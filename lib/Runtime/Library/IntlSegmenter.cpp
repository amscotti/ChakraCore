//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlSegmenter.h"

#ifdef INTL_ICU

// Ownership record (doc/memory-management.md section 3)
//
// Resource / creation call:
//   One UBreakIterator* per IntlSegmenterState (ubrk_open in the constructor), per IntlSegmentsState
//   (clone of the Segmenter's iterator in %Segmenter.prototype%.segment) and per IntlSegmentIteratorState
//   (clone of the Segments' iterator in %SegmentsPrototype%[@@iterator]).
// Domain / allocator:
//   ICU's own heap (ubrk_*); the JS-visible state objects are Recycler finalized objects
//   (RecyclerNewWithBarrierFinalized) reachable from the owning JS object's HiddenObject internal property.
// Owner / release authority:
//   Exactly the owning state object; released by ubrk_close in Dispose (idempotent, nulls the pointer and
//   invalidates the magic). Finalize() forwards to Dispose(). No alias of the pointer outlives the state.
// Borrowed views:
//   The UChar* of a Recycler JavascriptString is attached to a UBreakIterator ONLY inside FindSegmentBounds
//   (a synchronous leaf window with no JS calls and no Recycler allocation). The window always ends by
//   re-pointing the iterator at a static empty buffer, so no iterator ever retains a Recycler buffer across a
//   JS call or a GC. Result objects are allocated after the window has closed.
// Invalidation / reentrancy:
//   containing(index) runs ToInteger(index) (user code: valueOf, GC) BEFORE the string is read or any ICU text
//   is attached. next() and containing() are reentrant against one another because no ICU state is kept
//   between calls except the iterator's (re-attached) text, and each call re-derives positions from the
//   explicit index (iterator state keeps only an int32 index, never an ICU position).
// Thread/context:
//   Single script thread of the owning ScriptContext; finalization runs on the Recycler thread but touches only
//   the ICU handle (ubrk_close), never a JS object.
// Failure / OOM:
//   Failure paths allocate the state first with a null handle, then open/clone and publish the handle with one
//   non-throwing store; a failed open leaves a valid state that Dispose handles.
// Accounting:
//   ICU memory is not reported to the Recycler; each handle is small and bounded by the live state objects.

namespace Js
{
    namespace
    {
        enum class SegmentGranularity : uint8
        {
            Grapheme,
            Word,
            Sentence
        };

        UBreakIteratorType ToBreakIteratorType(SegmentGranularity granularity)
        {
            switch (granularity)
            {
            case SegmentGranularity::Word:
                return UBRK_WORD;
            case SegmentGranularity::Sentence:
                return UBRK_SENTENCE;
            default:
                return UBRK_CHARACTER;
            }
        }

        // Base for the three states: owns an optional UBreakIterator* and nothing else that needs release.
        class IntlSegmentBreakStateBase : public IntlModern::IntlModernState
        {
        private:
            FieldNoBarrier(UBreakIterator*) breakIterator;
            FieldNoBarrier(SegmentGranularity) granularity;

        protected:
            IntlSegmentBreakStateBase(IntlModern::IntlModernKind kind, SegmentGranularity granularity) :
                IntlModernState(kind),
                breakIterator(nullptr),
                granularity(granularity)
            {
            }

        public:
            UBreakIterator* GetBreakIterator() const { return breakIterator; }
            SegmentGranularity GetGranularity() const { return granularity; }

            // Publication point: a single non-throwing store taking ownership of bi.
            void AdoptBreakIterator(UBreakIterator* bi)
            {
                Assert(breakIterator == nullptr);
                breakIterator = bi;
            }

            virtual void Dispose(bool isShutdown) override
            {
                UBreakIterator* bi = breakIterator;
                breakIterator = nullptr;
                if (bi != nullptr)
                {
                    ubrk_close(bi);
                }
                InvalidateMagic();
            }
        };

        class IntlSegmenterState : public IntlSegmentBreakStateBase
        {
        private:
            Field(JavascriptString*) locale;
            Field(JavascriptString*) granularityString;
            Field(JavascriptString*) icuLocale;

        public:
            IntlSegmenterState(
                JavascriptString* locale,
                JavascriptString* granularityString,
                JavascriptString* icuLocale,
                SegmentGranularity granularity) :
                IntlSegmentBreakStateBase(IntlModern::IntlModernKind::Segmenter, granularity),
                locale(locale),
                granularityString(granularityString),
                icuLocale(icuLocale)
            {
            }

            JavascriptString* GetLocale() const { return locale; }
            JavascriptString* GetGranularityString() const { return granularityString; }
            JavascriptString* GetIcuLocale() const { return icuLocale; }
        };

        class IntlSegmentsState : public IntlSegmentBreakStateBase
        {
        private:
            Field(JavascriptString*) string;
            Field(JavascriptString*) icuLocale;

        public:
            IntlSegmentsState(JavascriptString* string, JavascriptString* icuLocale, SegmentGranularity granularity) :
                IntlSegmentBreakStateBase(IntlModern::IntlModernKind::Segments, granularity),
                string(string),
                icuLocale(icuLocale)
            {
            }

            JavascriptString* GetString() const { return string; }
            JavascriptString* GetIcuLocale() const { return icuLocale; }
        };

        class IntlSegmentIteratorState : public IntlSegmentBreakStateBase
        {
        private:
            Field(JavascriptString*) string;
            FieldNoBarrier(int32_t) index;

        public:
            IntlSegmentIteratorState(JavascriptString* string, SegmentGranularity granularity) :
                IntlSegmentBreakStateBase(IntlModern::IntlModernKind::SegmentIterator, granularity),
                string(string),
                index(0)
            {
            }

            JavascriptString* GetString() const { return string; }
            int32_t GetIndex() const { return index; }
            void SetIndex(int32_t value) { index = value; }
        };

        struct SegmentBounds
        {
            int32_t start;
            int32_t end;
            bool isWordLike;
        };

        void __declspec(noreturn) ThrowBreakIteratorFailure(ScriptContext* scriptContext, UErrorCode status)
        {
            if (status == U_MEMORY_ALLOCATION_ERROR)
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Failed to create ICU break iterator for Intl.Segmenter"));
        }

        // Opens a fresh iterator with no text attached. Throws on failure; nothing is owned on the throwing path.
        UBreakIterator* OpenBreakIterator(SegmentGranularity granularity, JavascriptString* icuLocale, ScriptContext* scriptContext)
        {
            IntlModern::ScopedAsciiString ascii;
            if (!ascii.Init(icuLocale))
            {
                if (ascii.IsOom())
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                ThrowBreakIteratorFailure(scriptContext, U_ILLEGAL_ARGUMENT_ERROR);
            }

            UErrorCode status = U_ZERO_ERROR;
            UBreakIterator* bi = ubrk_open(ToBreakIteratorType(granularity), ascii.Data(), nullptr, 0, &status);
            if (U_FAILURE(status) || bi == nullptr)
            {
                if (bi != nullptr)
                {
                    ubrk_close(bi);
                }
                ThrowBreakIteratorFailure(scriptContext, U_FAILURE(status) ? status : U_MEMORY_ALLOCATION_ERROR);
            }
            return bi;
        }

        // Clones prototypeBi (which has no Recycler text attached by invariant) when ICU supports it, otherwise
        // opens a fresh iterator for the same locale/granularity. Throws on failure.
        UBreakIterator* CloneOrOpenBreakIterator(
            UBreakIterator* prototypeBi,
            SegmentGranularity granularity,
            JavascriptString* icuLocale,
            ScriptContext* scriptContext)
        {
#if ICU_VERSION >= 69
            if (prototypeBi != nullptr)
            {
                UErrorCode status = U_ZERO_ERROR;
                UBreakIterator* clone = ubrk_clone(prototypeBi, &status);
                if (U_SUCCESS(status) && clone != nullptr)
                {
                    return clone;
                }
                if (clone != nullptr)
                {
                    ubrk_close(clone);
                }
            }
#else
            (void)prototypeBi;
#endif
            return OpenBreakIterator(granularity, icuLocale, scriptContext);
        }

        // Converts a canonical BCP 47 tag (ASCII) to an ICU locale id. Falls back to "en".
        JavascriptString* ToIcuLocaleString(JavascriptString* bcp47, ScriptContext* scriptContext)
        {
            IntlModern::ScopedAsciiString ascii;
            if (ascii.Init(bcp47))
            {
                char icuId[ULOC_FULLNAME_CAPACITY] = { 0 };
                int32_t parsed = 0;
                UErrorCode status = U_ZERO_ERROR;
                int32_t len = uloc_forLanguageTag(ascii.Data(), icuId, static_cast<int32_t>(sizeof(icuId)), &parsed, &status);
                if (U_SUCCESS(status) && !ICU_FAILURE(status) && len > 0 && static_cast<size_t>(len) < sizeof(icuId))
                {
                    return IntlModern::CreateAsciiString(icuId, static_cast<size_t>(len), scriptContext);
                }
            }
            else if (ascii.IsOom())
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            return IntlModern::CreateAsciiString("en", 2, scriptContext);
        }

        // The ONLY place a Recycler string buffer is attached to a UBreakIterator. Must not call JS, must not
        // allocate from the Recycler, and unconditionally detaches the text before returning.
        //
        // containing == true: [start, end) is the segment containing 'index'.
        // containing == false: 'index' is already a boundary; [start, end) is the segment beginning there.
        bool FindSegmentBounds(
            UBreakIterator* bi,
            const char16* text,
            int32_t length,
            int32_t index,
            bool containing,
            bool wantWordLike,
            SegmentBounds* out)
        {
            Assert(bi != nullptr && out != nullptr && index >= 0 && index < length);

            UErrorCode status = U_ZERO_ERROR;
            ubrk_setText(bi, reinterpret_cast<const UChar*>(text), length, &status);
            bool ok = U_SUCCESS(status);
            if (ok)
            {
                int32_t start = index;
                int32_t probe = index;
                if (containing)
                {
                    // ICU snaps a mid-surrogate-pair offset back to the pair's lead unit before searching, which
                    // would skip a boundary that sits at that lead unit. Normalize to a code point start first.
                    if (probe > 0 && U16_IS_TRAIL(text[probe]) && U16_IS_LEAD(text[probe - 1]))
                    {
                        --probe;
                    }
                    if (ubrk_isBoundary(bi, probe))
                    {
                        start = probe;
                    }
                    else
                    {
                        int32_t preceding = ubrk_preceding(bi, probe);
                        start = (preceding == UBRK_DONE) ? 0 : preceding;
                    }
                }
                int32_t end = ubrk_following(bi, probe);
                if (end == UBRK_DONE)
                {
                    end = length;
                }
                out->start = start;
                out->end = end;
                out->isWordLike = wantWordLike && ubrk_getRuleStatus(bi) >= UBRK_WORD_NONE_LIMIT;
            }

            // Unconditional detach: never leave the Recycler buffer attached to the iterator.
            static const UChar kEmpty = 0;
            UErrorCode resetError = U_ZERO_ERROR;
            ubrk_setText(bi, &kEmpty, 0, &resetError);
            return ok;
        }

        // Builds { segment, index, input[, isWordLike] }. Allocates; the iterator must be detached already.
        // 'text' is the flat buffer of 'input', which is kept alive by the caller's state/pinned reference.
        DynamicObject* CreateSegmentDataObject(
            ScriptContext* scriptContext,
            JavascriptString* input,
            const char16* text,
            const SegmentBounds& bounds,
            bool includeWordLike)
        {
            JavascriptLibrary* library = scriptContext->GetLibrary();
            JavascriptString* segment = IntlModern::CreateUtf16String(
                reinterpret_cast<const UChar*>(text + bounds.start), bounds.end - bounds.start, scriptContext);
            DynamicObject* result = IntlModern::CreatePlainDataObject(scriptContext);
            IntlModern::SetDataProperty(result, _u("segment"), segment, scriptContext);
            IntlModern::SetDataProperty(result, _u("index"), JavascriptNumber::ToVar(static_cast<int32>(bounds.start), scriptContext), scriptContext);
            IntlModern::SetDataProperty(result, _u("input"), input, scriptContext);
            if (includeWordLike)
            {
                IntlModern::SetDataProperty(result, _u("isWordLike"), bounds.isWordLike ? library->GetTrue() : library->GetFalse(), scriptContext);
            }
            return result;
        }

        void PublishState(ScriptContext* scriptContext, DynamicObject* obj, IntlModern::IntlModernState* state)
        {
            if (!IntlModern::SetState(obj, state))
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
        }

        bool GetGranularityFromString(JavascriptString* value, SegmentGranularity* out)
        {
            struct Entry { const char16* name; SegmentGranularity granularity; };
            static const Entry entries[] =
            {
                { _u("grapheme"), SegmentGranularity::Grapheme },
                { _u("word"), SegmentGranularity::Word },
                { _u("sentence"), SegmentGranularity::Sentence },
            };
            const char16* sz = value->GetString();
            charcount_t len = value->GetLength();
            for (size_t i = 0; i < _countof(entries); ++i)
            {
                if (len == PAL_wcslen(entries[i].name) && wmemcmp(sz, entries[i].name, len) == 0)
                {
                    *out = entries[i].granularity;
                    return true;
                }
            }
            return false;
        }
    } // namespace

    void IntlSegmenter::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        RuntimeFunction* ctor = nullptr;
        DynamicObject* proto = IntlModern::RegisterIntlClass(
            library,
            intlObject,
            _u("Segmenter"),
            &EntryInfo::NewInstance,
            0,
            _u("Intl.Segmenter"),
            nullptr,
            &ctor);

        library->SetIntlSegmenterPrototype(proto);

        library->AddFunctionToLibraryObject(ctor, PropertyIds::supportedLocalesOf, &EntryInfo::SupportedLocalesOf, 1);
        IntlModern::AddIntlMethod(library, proto, _u("segment"), &EntryInfo::Segment, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::resolvedOptions, &EntryInfo::ResolvedOptions, 0);

        DynamicObject* segmentsProto = library->CreateObject();
        IntlModern::AddIntlMethod(library, segmentsProto, _u("containing"), &EntryInfo::SegmentsContaining, 1);
        library->AddFunctionToLibraryObjectWithName(
            segmentsProto,
            PropertyIds::_symbolIterator,
            PropertyIds::_RuntimeFunctionNameId_iterator,
            &EntryInfo::SegmentsIterator,
            0);
        library->SetIntlSegmentsPrototype(segmentsProto);

        DynamicObject* segIterProto = library->CreateObject(library->GetIteratorPrototype());
        library->AddFunctionToLibraryObject(segIterProto, PropertyIds::next, &EntryInfo::SegmentIteratorNext, 0);
        library->AddMember(
            segIterProto,
            PropertyIds::_symbolToStringTag,
            library->CreateStringFromCppLiteral(_u("Segmenter String Iterator")),
            PropertyConfigurable);
        library->SetIntlSegmentIteratorPrototype(segIterProto);
    }

    Var IntlSegmenter::EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.Segmenter"));
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.Segmenter"));
        }
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.Segmenter"));
        }

        JavascriptLibrary* library = scriptContext->GetLibrary();
        Var undefinedVar = library->GetUndefined();
        Var locales = args.Info.Count > 1 ? args[1] : undefinedVar;
        Var options = args.Info.Count > 2 ? args[2] : undefinedVar;

        DynamicObject* obj = IntlModern::OrdinaryCreateFromConstructor(newTarget, scriptContext, &JavascriptLibrary::GetIntlSegmenterPrototype);

        JavascriptArray* requestedLocales = IntlModern::CanonicalizeLocaleList(locales, scriptContext);
        RecyclableObject* optionsObject = IntlModern::GetOptionsObject(options, scriptContext);

        static const char16* const matcherValues[] = { _u("lookup"), _u("best fit") };
        JavascriptString* matcher = IntlModern::GetStringOption(
            optionsObject,
            PropertyIds::localeMatcher,
            _u("localeMatcher"),
            matcherValues,
            _countof(matcherValues),
            _u("best fit"),
            scriptContext);

        IntlModern::ResolvedLocale resolved = IntlModern::ResolveLocale(
            IntlModern::IntlModernKind::Segmenter,
            requestedLocales,
            matcher->GetString(),
            nullptr,
            0,
            nullptr,
            scriptContext);

        static const char16* const granularityValues[] = { _u("grapheme"), _u("word"), _u("sentence") };
        JavascriptString* granularityString = IntlModern::GetStringOption(
            optionsObject,
            _u("granularity"),
            granularityValues,
            _countof(granularityValues),
            _u("grapheme"),
            scriptContext);

        SegmentGranularity granularity = SegmentGranularity::Grapheme;
        if (!GetGranularityFromString(granularityString, &granularity))
        {
            // GetStringOption already validated against granularityValues.
            Assert(false);
        }

        JavascriptString* icuLocale = ToIcuLocaleString(resolved.locale, scriptContext);

        IntlSegmenterState* state = RecyclerNewWithBarrierFinalized(
            scriptContext->GetRecycler(), IntlSegmenterState, resolved.locale, granularityString, icuLocale, granularity);
        state->AdoptBreakIterator(OpenBreakIterator(granularity, icuLocale, scriptContext));
        PublishState(scriptContext, obj, state);
        return obj;
    }

    Var IntlSegmenter::EntrySupportedLocalesOf(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.Segmenter.supportedLocalesOf"));

        Var undefinedVar = scriptContext->GetLibrary()->GetUndefined();
        Var locales = args.Info.Count > 1 ? args[1] : undefinedVar;
        Var options = args.Info.Count > 2 ? args[2] : undefinedVar;
        return IntlModern::SupportedLocales(IntlModern::IntlModernKind::Segmenter, locales, options, scriptContext);
    }

    Var IntlSegmenter::EntrySegment(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.Segmenter.prototype.segment"));

        IntlSegmenterState* segmenter = IntlModern::RequireState<IntlSegmenterState>(
            args[0], IntlModern::IntlModernKind::Segmenter, scriptContext, _u("Intl.Segmenter.prototype.segment"));

        JavascriptLibrary* library = scriptContext->GetLibrary();
        Var stringArg = args.Info.Count > 1 ? args[1] : library->GetUndefined();
        JavascriptString* string = JavascriptConversion::ToString(stringArg, scriptContext);
        // Flatten now so that later leaf windows never need to allocate to obtain the buffer.
        (void)string->GetString();

        SegmentGranularity granularity = segmenter->GetGranularity();
        JavascriptString* icuLocale = segmenter->GetIcuLocale();

        DynamicObject* obj = library->CreateObject(library->GetIntlSegmentsPrototype());
        IntlSegmentsState* state = RecyclerNewWithBarrierFinalized(
            scriptContext->GetRecycler(), IntlSegmentsState, string, icuLocale, granularity);
        state->AdoptBreakIterator(CloneOrOpenBreakIterator(segmenter->GetBreakIterator(), granularity, icuLocale, scriptContext));
        PublishState(scriptContext, obj, state);
        return obj;
    }

    Var IntlSegmenter::EntryResolvedOptions(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.Segmenter.prototype.resolvedOptions"));

        IntlSegmenterState* segmenter = IntlModern::RequireState<IntlSegmenterState>(
            args[0], IntlModern::IntlModernKind::Segmenter, scriptContext, _u("Intl.Segmenter.prototype.resolvedOptions"));

        DynamicObject* result = IntlModern::CreatePlainDataObject(scriptContext);
        IntlModern::SetDataProperty(result, _u("locale"), segmenter->GetLocale(), scriptContext);
        IntlModern::SetDataProperty(result, _u("granularity"), segmenter->GetGranularityString(), scriptContext);
        return result;
    }

    Var IntlSegmenter::EntrySegmentsContaining(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.Segmenter.%Segments%.prototype.containing"));

        IntlSegmentsState* segments = IntlModern::RequireState<IntlSegmentsState>(
            args[0], IntlModern::IntlModernKind::Segments, scriptContext, _u("%Segments%.prototype.containing"));

        JavascriptLibrary* library = scriptContext->GetLibrary();
        Var indexArg = args.Info.Count > 1 ? args[1] : library->GetUndefined();

        // User code (valueOf) and GC may run here. No string buffer has been touched or attached to ICU yet.
        double n = JavascriptConversion::ToInteger(indexArg, scriptContext);

        JavascriptString* string = segments->GetString();
        charcount_t length = string->GetLength();
        if (n < 0 || n >= static_cast<double>(length))
        {
            return library->GetUndefined();
        }

        Var result = nullptr;
        ENTER_PINNED_SCOPE(JavascriptString, pinnedString);
        pinnedString = string;

        const char16* text = pinnedString->GetString();
        UBreakIterator* bi = segments->GetBreakIterator();
        bool isWord = segments->GetGranularity() == SegmentGranularity::Word;

        SegmentBounds bounds = { 0, 0, false };
        bool ok = bi != nullptr && FindSegmentBounds(bi, text, static_cast<int32_t>(length), static_cast<int32_t>(n), true, isWord, &bounds);
        if (!ok)
        {
            ThrowBreakIteratorFailure(scriptContext, U_INVALID_STATE_ERROR);
        }
        result = CreateSegmentDataObject(scriptContext, pinnedString, text, bounds, isWord);
        LEAVE_PINNED_SCOPE();
        return result;
    }

    Var IntlSegmenter::EntrySegmentsIterator(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.Segmenter.%Segments%.prototype[Symbol.iterator]"));

        IntlSegmentsState* segments = IntlModern::RequireState<IntlSegmentsState>(
            args[0], IntlModern::IntlModernKind::Segments, scriptContext, _u("%Segments%.prototype[Symbol.iterator]"));

        JavascriptLibrary* library = scriptContext->GetLibrary();
        SegmentGranularity granularity = segments->GetGranularity();

        DynamicObject* obj = library->CreateObject(library->GetIntlSegmentIteratorPrototype());
        IntlSegmentIteratorState* state = RecyclerNewWithBarrierFinalized(
            scriptContext->GetRecycler(), IntlSegmentIteratorState, segments->GetString(), granularity);
        state->AdoptBreakIterator(
            CloneOrOpenBreakIterator(segments->GetBreakIterator(), granularity, segments->GetIcuLocale(), scriptContext));
        PublishState(scriptContext, obj, state);
        return obj;
    }

    Var IntlSegmenter::EntrySegmentIteratorNext(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("%SegmentIterator%.prototype.next"));

        IntlSegmentIteratorState* iterator = IntlModern::RequireState<IntlSegmentIteratorState>(
            args[0], IntlModern::IntlModernKind::SegmentIterator, scriptContext, _u("%SegmentIterator%.prototype.next"));

        JavascriptLibrary* library = scriptContext->GetLibrary();
        JavascriptString* string = iterator->GetString();
        int32_t length = static_cast<int32_t>(string->GetLength());
        int32_t index = iterator->GetIndex();
        if (index >= length)
        {
            return library->CreateIteratorResultObjectDone();
        }

        Var result = nullptr;
        ENTER_PINNED_SCOPE(JavascriptString, pinnedString);
        pinnedString = string;

        const char16* text = pinnedString->GetString();
        UBreakIterator* bi = iterator->GetBreakIterator();
        bool isWord = iterator->GetGranularity() == SegmentGranularity::Word;

        SegmentBounds bounds = { 0, 0, false };
        bool ok = bi != nullptr && FindSegmentBounds(bi, text, length, index, false, isWord, &bounds);
        if (!ok)
        {
            ThrowBreakIteratorFailure(scriptContext, U_INVALID_STATE_ERROR);
        }
        DynamicObject* value = CreateSegmentDataObject(scriptContext, pinnedString, text, bounds, isWord);
        iterator->SetIndex(bounds.end);
        result = library->CreateIteratorResultObject(value, false);
        LEAVE_PINNED_SCOPE();
        return result;
    }
} // namespace Js

#endif // INTL_ICU
