//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlListFormat.h"

#ifdef INTL_ICU

// Ownership record (doc/memory-management.md section 3):
//
//   Resource:   UListFormatter* from ulistfmt_openForType (ICU heap).
//   Owner:      IntlListFormatState (Recycler finalized object, WithBarrier), published on the
//               Intl.ListFormat instance through IntlModern::SetState (HiddenObject).
//   Release:    ulistfmt_close in Dispose(); Dispose is idempotent (pointer nulled, magic
//               invalidated) because the Recycler may call Finalize and Dispose for one object.
//   Partial failure: the handle is wrapped in the state immediately after open, so any later
//               throw leaves an unpublished state that the Recycler finalizes.
//   Retained JS pointers: locale/type/style, each in a Field() write-barrier slot.
//   Reentrancy: StringListFromIterable runs user code (iterator protocol). The collected strings
//               live in a JavascriptArray (GC-visible). The formatter and every string view are
//               only acquired after the last user-visible step; ICU calls never run JS.
//   Thread/context: single script thread; ICU objects are used only inside synchronous entry points.

namespace Js
{
    namespace
    {
        class IntlListFormatState : public IntlModern::IntlModernState
        {
        private:
            FieldNoBarrier(UListFormatter*) formatter;
            Field(JavascriptString*) locale;
            Field(JavascriptString*) type;
            Field(JavascriptString*) style;

        public:
            IntlListFormatState(UListFormatter* formatter, JavascriptString* locale, JavascriptString* type, JavascriptString* style) :
                IntlModern::IntlModernState(IntlModern::IntlModernKind::ListFormat),
                formatter(formatter),
                locale(locale),
                type(type),
                style(style)
            {
            }

            static IntlListFormatState* New(
                Recycler* recycler,
                UListFormatter* formatter,
                JavascriptString* locale,
                JavascriptString* type,
                JavascriptString* style)
            {
                return RecyclerNewWithBarrierFinalized(recycler, IntlListFormatState, formatter, locale, type, style);
            }

            UListFormatter* GetFormatter() const { return formatter; }
            JavascriptString* GetLocale() const { return locale; }
            JavascriptString* GetType() const { return type; }
            JavascriptString* GetStyle() const { return style; }

            virtual JavascriptString* GetLocaleTag() const override { return locale; }

            virtual void Dispose(bool isShutdown) override
            {
                if (formatter != nullptr)
                {
                    ulistfmt_close(formatter);
                    formatter = nullptr;
                }
                InvalidateMagic();
            }
        };

        // Native scratch for the (pointer, length) pairs handed to ICU. Not a GC object.
        class ScopedStringViews
        {
        private:
            const UChar** strings;
            int32_t* lengths;
            uint32 count;

        public:
            ScopedStringViews() : strings(nullptr), lengths(nullptr), count(0) {}

            ~ScopedStringViews()
            {
                if (strings != nullptr)
                {
                    NoCheckHeapDeleteArray(count, strings);
                }
                if (lengths != nullptr)
                {
                    NoCheckHeapDeleteArray(count, lengths);
                }
            }

            bool Init(uint32 n)
            {
                count = n;
                if (n == 0)
                {
                    return true;
                }
                strings = NoCheckHeapNewArray(const UChar*, n);
                lengths = NoCheckHeapNewArray(int32_t, n);
                if (strings == nullptr || lengths == nullptr)
                {
                    return false;
                }
                return true;
            }

            const UChar** Strings() const { return strings; }
            int32_t* Lengths() const { return lengths; }
        };

        void ThrowIcuFailure(ScriptContext* scriptContext, UErrorCode status)
        {
            if (status == U_MEMORY_ALLOCATION_ERROR)
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.ListFormat: internal ICU failure"));
        }

        bool StringEquals(JavascriptString* str, const char16* literal)
        {
            return PAL_wcscmp(str->GetSz(), literal) == 0;
        }

        // ICU locale ids ("en_US") from a canonical BCP 47 tag ("en-US").
        void ToIcuLocaleId(JavascriptString* tag, char* out, int32_t cap, ScriptContext* scriptContext)
        {
            IntlModern::ScopedAsciiString ascii;
            if (!ascii.Init(tag))
            {
                if (ascii.IsOom())
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                IntlModern::ThrowLocaleRangeError(scriptContext, tag);
            }
            UErrorCode status = U_ZERO_ERROR;
            int32_t parsed = 0;
            uloc_forLanguageTag(ascii.Data(), out, cap, &parsed, &status);
            if (ICU_FAILURE(status))
            {
                ThrowIcuFailure(scriptContext, status);
            }
        }

        static const char16* const gLocaleMatcherValues[] = { _u("lookup"), _u("best fit") };
        static const char16* const gTypeValues[] = { _u("conjunction"), _u("disjunction"), _u("unit") };
        static const char16* const gStyleValues[] = { _u("long"), _u("short"), _u("narrow") };

        // StringListFromIterable. User code (iterator protocol) runs here; the result array keeps the
        // strings reachable. A non-String element is a TypeError and closes the iterator
        // (DoIteratorStepAndValue calls IteratorClose only for errors raised after a successful step).
        JavascriptArray* StringListFromIterable(Var iterable, ScriptContext* scriptContext)
        {
            JavascriptArray* list = scriptContext->GetLibrary()->CreateArray(0);
            if (JavascriptOperators::IsUndefined(iterable))
            {
                return list;
            }

            RecyclableObject* iterator = JavascriptOperators::GetIterator(iterable, scriptContext);
            uint32 count = 0;
            JavascriptOperators::DoIteratorStepAndValue(iterator, scriptContext, [&](Var next)
            {
                if (!VarIs<JavascriptString>(next))
                {
                    JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedString, _u("Intl.ListFormat list element"));
                }
                if (count == UINT32_MAX - 1)
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                list->DirectSetItemAt(count, next);
                ++count;
            });
            return list;
        }

        // Formats the (already validated) list. Everything JS-visible has completed; `list` only holds
        // JavascriptString elements. The caller owns result; this acquires the string views.
        UFormattedList* FormatList(
            UListFormatter* formatter,
            JavascriptArray* list,
            const UChar** outValueChars,
            int32_t* outValueLen,
            ScriptContext* scriptContext)
        {
            uint32 count = list->GetLength();
            ScopedStringViews views;
            if (!views.Init(count))
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            for (uint32 i = 0; i < count; ++i)
            {
                JavascriptString* s = VarTo<JavascriptString>(list->DirectGetItem(i));
                views.Strings()[i] = reinterpret_cast<const UChar*>(s->GetString());
                views.Lengths()[i] = static_cast<int32_t>(s->GetLength());
            }

            UErrorCode status = U_ZERO_ERROR;
            UFormattedList* result = ulistfmt_openResult(&status);
            if (ICU_FAILURE(status) || result == nullptr)
            {
                ThrowIcuFailure(scriptContext, status);
            }
            ulistfmt_formatStringsToResult(formatter, views.Strings(), views.Lengths(), static_cast<int32_t>(count), result, &status);
            if (ICU_FAILURE(status))
            {
                ulistfmt_closeResult(result);
                ThrowIcuFailure(scriptContext, status);
            }
            const UFormattedValue* value = ulistfmt_resultAsValue(result, &status);
            const UChar* chars = ICU_FAILURE(status) ? nullptr : ufmtval_getString(value, outValueLen, &status);
            if (ICU_FAILURE(status) || chars == nullptr)
            {
                ulistfmt_closeResult(result);
                ThrowIcuFailure(scriptContext, status);
            }
            *outValueChars = chars;
            return result;
        }

        UListFormatter* GetLiveFormatter(IntlListFormatState* state, ScriptContext* scriptContext)
        {
            UListFormatter* formatter = state->IsValidState() ? state->GetFormatter() : nullptr;
            if (formatter == nullptr)
            {
                IntlModern::ThrowInvalidReceiverTypeError(scriptContext, _u("Intl.ListFormat"));
            }
            return formatter;
        }
    } // namespace

    void IntlListFormat::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        RuntimeFunction* ctor = nullptr;
        DynamicObject* proto = IntlModern::RegisterIntlClass(
            library,
            intlObject,
            _u("ListFormat"),
            &EntryInfo::NewInstance,
            0,
            _u("Intl.ListFormat"),
            nullptr,
            &ctor);

        library->SetIntlListFormatPrototype(proto);

        library->AddFunctionToLibraryObject(ctor, PropertyIds::supportedLocalesOf, &EntryInfo::SupportedLocalesOf, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::format, &EntryInfo::Format, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::formatToParts, &EntryInfo::FormatToParts, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::resolvedOptions, &EntryInfo::ResolvedOptions, 0);
    }

    Var IntlListFormat::EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.ListFormat"));
        }
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.ListFormat"));
        }

        Var locales = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        Var optionsArg = args.Info.Count > 2 ? args[2] : scriptContext->GetLibrary()->GetUndefined();

        // 1. OrdinaryCreateFromConstructor (may run user code through newTarget.prototype).
        DynamicObject* listFormat = IntlModern::OrdinaryCreateFromConstructor(newTarget, scriptContext, &JavascriptLibrary::GetIntlListFormatPrototype);

        // 2-3. CanonicalizeLocaleList, GetOptionsObject.
        JavascriptArray* requestedLocales = IntlModern::CanonicalizeLocaleList(locales, scriptContext);
        RecyclableObject* options = IntlModern::GetOptionsObject(optionsArg, scriptContext);

        // 4-8. Option reads in spec order: localeMatcher, type, style.
        JavascriptString* matcher = IntlModern::GetStringOption(
            options, _u("localeMatcher"), gLocaleMatcherValues, _countof(gLocaleMatcherValues), _u("best fit"), scriptContext);
        JavascriptString* type = IntlModern::GetStringOption(
            options, _u("type"), gTypeValues, _countof(gTypeValues), _u("conjunction"), scriptContext);
        JavascriptString* style = IntlModern::GetStringOption(
            options, _u("style"), gStyleValues, _countof(gStyleValues), _u("long"), scriptContext);

        IntlModern::ResolvedLocale resolved = IntlModern::ResolveLocale(
            IntlModern::IntlModernKind::ListFormat,
            requestedLocales,
            matcher->GetSz(),
            nullptr,
            0,
            nullptr,
            scriptContext);

        char icuLocale[ULOC_FULLNAME_CAPACITY] = { 0 };
        ToIcuLocaleId(resolved.dataLocale, icuLocale, static_cast<int32_t>(sizeof(icuLocale)), scriptContext);

        UListFormatterType icuType = ULISTFMT_TYPE_AND;
        if (StringEquals(type, _u("disjunction")))
        {
            icuType = ULISTFMT_TYPE_OR;
        }
        else if (StringEquals(type, _u("unit")))
        {
            icuType = ULISTFMT_TYPE_UNITS;
        }

        UListFormatterWidth icuWidth = ULISTFMT_WIDTH_WIDE;
        if (StringEquals(style, _u("short")))
        {
            icuWidth = ULISTFMT_WIDTH_SHORT;
        }
        else if (StringEquals(style, _u("narrow")))
        {
            icuWidth = ULISTFMT_WIDTH_NARROW;
        }

        UErrorCode status = U_ZERO_ERROR;
        UListFormatter* formatter = ulistfmt_openForType(icuLocale, icuType, icuWidth, &status);
        if (ICU_FAILURE(status) || formatter == nullptr)
        {
            if (formatter != nullptr)
            {
                ulistfmt_close(formatter);
            }
            ThrowIcuFailure(scriptContext, status);
        }

        // Ownership of `formatter` transfers to the state here. If allocation of the state throws
        // (OOM), nothing owns the handle yet, so close it explicitly.
        IntlListFormatState* state = nullptr;
        try
        {
            state = IntlListFormatState::New(scriptContext->GetRecycler(), formatter, resolved.locale, type, style);
        }
        catch (...)
        {
            ulistfmt_close(formatter);
            throw;
        }

        if (!IntlModern::SetState(listFormat, state))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DefineProperty_NotExtensible, _u("Intl.ListFormat"));
        }
        return listFormat;
    }

    Var IntlListFormat::EntrySupportedLocalesOf(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.ListFormat.supportedLocalesOf"));
        Assert(!(callInfo.Flags & CallFlags_New));

        Var undefined = scriptContext->GetLibrary()->GetUndefined();
        Var locales = args.Info.Count > 1 ? args[1] : undefined;
        Var options = args.Info.Count > 2 ? args[2] : undefined;
        return IntlModern::SupportedLocales(IntlModern::IntlModernKind::ListFormat, locales, options, scriptContext);
    }

    Var IntlListFormat::EntryFormat(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.ListFormat.prototype.format"));
        Assert(!(callInfo.Flags & CallFlags_New));

        IntlListFormatState* state = IntlModern::RequireState<IntlListFormatState>(
            args.Info.Count > 0 ? args[0] : scriptContext->GetLibrary()->GetUndefined(),
            IntlModern::IntlModernKind::ListFormat,
            scriptContext,
            _u("Intl.ListFormat.prototype.format"));

        Var iterable = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        JavascriptArray* list = StringListFromIterable(iterable, scriptContext);

        // Reacquire native state after all user code.
        UListFormatter* formatter = GetLiveFormatter(state, scriptContext);
        const UChar* chars = nullptr;
        int32_t len = 0;
        UFormattedList* result = FormatList(formatter, list, &chars, &len, scriptContext);
        PlatformAgnostic::ICUHelpers::ScopedUFormattedList scopedResult(result);
        return IntlModern::CreateUtf16String(chars, len, scriptContext);
    }

    Var IntlListFormat::EntryFormatToParts(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.ListFormat.prototype.formatToParts"));
        Assert(!(callInfo.Flags & CallFlags_New));

        IntlListFormatState* state = IntlModern::RequireState<IntlListFormatState>(
            args.Info.Count > 0 ? args[0] : scriptContext->GetLibrary()->GetUndefined(),
            IntlModern::IntlModernKind::ListFormat,
            scriptContext,
            _u("Intl.ListFormat.prototype.formatToParts"));

        Var iterable = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        JavascriptArray* list = StringListFromIterable(iterable, scriptContext);

        UListFormatter* formatter = GetLiveFormatter(state, scriptContext);
        const UChar* chars = nullptr;
        int32_t len = 0;
        UFormattedList* result = FormatList(formatter, list, &chars, &len, scriptContext);
        PlatformAgnostic::ICUHelpers::ScopedUFormattedList scopedResult(result);

        UErrorCode status = U_ZERO_ERROR;
        const UFormattedValue* value = ulistfmt_resultAsValue(result, &status);
        PlatformAgnostic::ICUHelpers::ScopedUConstrainedFieldPosition pos(ucfpos_open(&status));
        if (ICU_FAILURE(status) || value == nullptr)
        {
            ThrowIcuFailure(scriptContext, status);
        }
        ucfpos_constrainCategory(pos, UFIELD_CATEGORY_LIST, &status);
        if (ICU_FAILURE(status))
        {
            ThrowIcuFailure(scriptContext, status);
        }

        JavascriptLibrary* library = scriptContext->GetLibrary();
        JavascriptArray* parts = library->CreateArray(0);
        uint32 partCount = 0;
        int32_t cursor = 0;

        auto addPart = [&](bool isElement, int32_t start, int32_t limit)
        {
            DynamicObject* part = IntlModern::CreatePlainDataObject(scriptContext);
            IntlModern::SetDataProperty(part, _u("type"), IntlModern::CreateAsciiString(isElement ? "element" : "literal", scriptContext), scriptContext);
            IntlModern::SetDataProperty(part, _u("value"), IntlModern::CreateUtf16String(chars + start, limit - start, scriptContext), scriptContext);
            parts->DirectSetItemAt(partCount++, static_cast<Var>(part));
        };

        while (ufmtval_nextPosition(value, pos, &status))
        {
            int32_t field = ucfpos_getField(pos, &status);
            int32_t start = 0;
            int32_t limit = 0;
            ucfpos_getIndexes(pos, &start, &limit, &status);
            if (ICU_FAILURE(status))
            {
                ThrowIcuFailure(scriptContext, status);
            }
            if (start < cursor || limit < start || limit > len)
            {
                continue;
            }
            if (start > cursor)
            {
                addPart(false, cursor, start);
            }
            if (limit > start)
            {
                addPart(field == ULISTFMT_ELEMENT_FIELD, start, limit);
            }
            cursor = limit;
        }
        if (ICU_FAILURE(status))
        {
            ThrowIcuFailure(scriptContext, status);
        }
        if (cursor < len)
        {
            addPart(false, cursor, len);
        }
        return parts;
    }

    Var IntlListFormat::EntryResolvedOptions(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.ListFormat.prototype.resolvedOptions"));
        Assert(!(callInfo.Flags & CallFlags_New));

        IntlListFormatState* state = IntlModern::RequireState<IntlListFormatState>(
            args.Info.Count > 0 ? args[0] : scriptContext->GetLibrary()->GetUndefined(),
            IntlModern::IntlModernKind::ListFormat,
            scriptContext,
            _u("Intl.ListFormat.prototype.resolvedOptions"));

        DynamicObject* result = IntlModern::CreatePlainDataObject(scriptContext);
        IntlModern::SetDataProperty(result, _u("locale"), state->GetLocale(), scriptContext);
        IntlModern::SetDataProperty(result, _u("type"), state->GetType(), scriptContext);
        IntlModern::SetDataProperty(result, _u("style"), state->GetStyle(), scriptContext);
        return result;
    }
} // namespace Js

#endif // INTL_ICU
