//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#ifdef INTL_ICU
#include "PlatformAgnostic/ChakraICU.h"
#endif

namespace Js
{
#ifdef INTL_ICU
    class IntlModernBuiltIns
    {
    public:
        class EntryInfo
        {
        public:
            static FunctionInfo GetCanonicalLocales;
        };

        static Var EntryGetCanonicalLocales(RecyclableObject* function, CallInfo callInfo, ...);

        static DynamicObject* RegisterClassImpl(
            JavascriptLibrary* library,
            DynamicObject* intlObject,
            const char16* className,
            FunctionInfo* ctorInfo,
            int length,
            const char16* toStringTag,
            DynamicObject* prototypeParent,
            RuntimeFunction** outCtor);
        static JavascriptFunction* AddMethodImpl(
            JavascriptLibrary* library,
            DynamicObject* obj,
            const char16* methodName,
            FunctionInfo* functionInfo,
            int length);
        static RuntimeFunction* AddGetterImpl(
            JavascriptLibrary* library,
            DynamicObject* obj,
            const char16* propName,
            FunctionInfo* functionInfo);
    };

    namespace IntlModern
    {
        enum class IntlModernKind : uint8
        {
            Locale,
            ListFormat,
            RelativeTimeFormat,
            DisplayNames,
            Segmenter,
            Segments,
            SegmentIterator,
            DurationFormat
        };

        class IntlModernState : public FinalizableObject
        {
        private:
            FieldNoBarrier(const void*) magic;
            FieldNoBarrier(IntlModernKind) kind;

        protected:
            IntlModernState(IntlModernKind kind);

        public:
            bool IsValidState() const;
            IntlModernKind GetKind() const { return kind; }
            void InvalidateMagic();

            virtual JavascriptString* GetLocaleTag() const { return nullptr; }
            virtual void Finalize(bool isShutdown) override { Dispose(isShutdown); }
            virtual void Dispose(bool isShutdown) override { InvalidateMagic(); }
            virtual void Mark(Recycler* recycler) override { AssertMsg(false, "Mark not implemented"); }
        };

        bool SetState(DynamicObject* obj, IntlModernState* state);
        IntlModernState* TryGetRawState(Var val, IntlModernKind kind);
        void ThrowInvalidReceiverTypeError(ScriptContext* scriptContext, const char16* methodName);

        template <typename T>
        inline T* TryGetState(Var val, IntlModernKind kind)
        {
            return static_cast<T*>(TryGetRawState(val, kind));
        }

        template <typename T>
        inline T* RequireState(Var val, IntlModernKind kind, ScriptContext* scriptContext, const char16* methodName)
        {
            T* state = TryGetState<T>(val, kind);
            if (state == nullptr)
            {
                ThrowInvalidReceiverTypeError(scriptContext, methodName);
            }
            return state;
        }

        JavascriptString* GetLocaleTagFromState(IntlModernState* localeState);
        JavascriptString* TryGetLocaleTagIfLocaleObject(Var val);

        DynamicObject* OrdinaryCreateFromConstructor(
            Var newTarget,
            ScriptContext* scriptContext,
            DynamicObject* (JavascriptLibrary::*getProto)() const);

        DynamicObject* RegisterIntlClass(
            JavascriptLibrary* library,
            DynamicObject* intlObject,
            const char16* className,
            FunctionInfo* ctorInfo,
            int length,
            const char16* toStringTag,
            DynamicObject* prototypeParent = nullptr,
            RuntimeFunction** outCtor = nullptr);

        JavascriptFunction* AddIntlMethod(
            JavascriptLibrary* library,
            DynamicObject* obj,
            const char16* methodName,
            FunctionInfo* functionInfo,
            int length);

        RuntimeFunction* AddIntlGetter(
            JavascriptLibrary* library,
            DynamicObject* obj,
            const char16* propName,
            FunctionInfo* functionInfo);

        DynamicObject* CreatePlainDataObject(ScriptContext* scriptContext);
        void SetDataProperty(DynamicObject* obj, PropertyId propId, Var value);
        void SetDataProperty(DynamicObject* obj, const char16* propName, Var value, ScriptContext* scriptContext);

        JavascriptString* CreateAsciiString(const char* ascii, size_t len, ScriptContext* scriptContext);
        JavascriptString* CreateAsciiString(const char* ascii, ScriptContext* scriptContext);
        JavascriptString* CreateUtf16String(const UChar* utf16, int32_t len, ScriptContext* scriptContext);

        class ScopedAsciiString
        {
        private:
            char stackBuf[256];
            char* buf;
            size_t len;
            bool isAscii;
            bool oom;

        public:
            ScopedAsciiString();
            ~ScopedAsciiString();

            bool Init(JavascriptString* str);
            const char* Data() const { return buf; }
            size_t Length() const { return len; }
            bool IsAscii() const { return isAscii; }
            bool IsOom() const { return oom; }
        };

        void __declspec(noreturn) ThrowRangeErrorMessage(ScriptContext* scriptContext, const char16* message);
        void __declspec(noreturn) ThrowLocaleRangeError(ScriptContext* scriptContext, JavascriptString* tag);
        void __declspec(noreturn) ThrowOptionRangeError(
            ScriptContext* scriptContext,
            const char16* optionName,
            JavascriptString* optionValue,
            const char16* const* expectedValues,
            size_t expectedCount);

        bool IsAsciiAlpha(char ch);
        bool IsAsciiDigit(char ch);
        bool IsAsciiAlnum(char ch);
        char ToAsciiLower(char ch);
        char ToAsciiUpper(char ch);

        bool IsUnicodeLanguageSubtag(const char* sub, size_t len);
        bool IsUnicodeScriptSubtag(const char* sub, size_t len);
        bool IsUnicodeRegionSubtag(const char* sub, size_t len);
        bool IsUnicodeVariantSubtag(const char* sub, size_t len);
        bool IsUnicodeTypeNonterminal(const char* str, size_t len);
        bool IsValidUnicodeLanguageId(const char* tag, size_t len);
        bool IsStructurallyValidUnicodeLanguageTag(const char* tag, size_t len);

        bool TryCanonicalizeUnicodeLocaleId(
            const char* tag,
            size_t len,
            ScriptContext* scriptContext,
            JavascriptString** outCanonical);
        JavascriptString* CanonicalizeUnicodeLocaleId(const char* tag, size_t len, ScriptContext* scriptContext);
        JavascriptString* ValidateAndCanonicalizeUnicodeLocaleId(JavascriptString* tag, ScriptContext* scriptContext);

        JavascriptString* MaximizeLocaleId(const char* canonicalTag, size_t len, ScriptContext* scriptContext);
        JavascriptString* MinimizeLocaleId(const char* canonicalTag, size_t len, ScriptContext* scriptContext);

        JavascriptArray* CanonicalizeLocaleList(Var locales, ScriptContext* scriptContext);

        RecyclableObject* GetOptionsObject(Var options, ScriptContext* scriptContext);
        RecyclableObject* CoerceOptionsToObject(Var options, ScriptContext* scriptContext);

        JavascriptString* GetStringOption(
            RecyclableObject* options,
            PropertyId propId,
            const char16* optionName,
            const char16* const* values,
            size_t valueCount,
            const char16* fallback,
            ScriptContext* scriptContext);
        JavascriptString* GetStringOption(
            RecyclableObject* options,
            const char16* optionName,
            const char16* const* values,
            size_t valueCount,
            const char16* fallback,
            ScriptContext* scriptContext);

        int GetBoolOption(RecyclableObject* options, PropertyId propId, int fallback, ScriptContext* scriptContext);
        int GetBoolOption(RecyclableObject* options, const char16* propName, int fallback, ScriptContext* scriptContext);

        JavascriptString* GetDefaultLocale(ScriptContext* scriptContext);
        bool IsSupportedNumberingSystem(const char* nu, size_t len);

        struct ResolvedLocale
        {
            JavascriptString* locale;
            JavascriptString* dataLocale;
            JavascriptString* numberingSystem;
        };

        ResolvedLocale ResolveLocale(
            IntlModernKind kind,
            JavascriptArray* requestedLocales,
            const char16* localeMatcher,
            const char16* const* relevantExtensionKeys,
            size_t keyCount,
            const char16* overrideNumberingSystem,
            ScriptContext* scriptContext);

        JavascriptArray* SupportedLocales(
            IntlModernKind kind,
            Var locales,
            Var options,
            ScriptContext* scriptContext);
    }
#endif
}
