//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlLocale.h"

#ifdef INTL_ICU

// Ownership record (doc/memory-management.md section 3):
//   Resource:  IntlLocaleState (Recycler finalized object, one per Intl.Locale instance), created in
//              IntlLocale::EntryNewInstance / locale transforms, published via IntlModern::SetState.
//   Domain:    Recycler; the only retained GC reference is the Field(JavascriptString*) tag, written
//              through the write barrier and traced because the state is allocated WithBarrier.
//   Native:    No ICU handle or native buffer outlives a call. All ICU objects are ScopedICUObject
//              locals; ASCII scratch copies are stack/NoCheckHeap buffers owned by the local TagBuilder.
//   Borrows:   Every string view (ScopedAsciiString / TagBuilder) is a copy made AFTER the last
//              user-visible coercion (ToString, ToObject, option getters) of the value it copies.
//   Reentrancy: only EntryNewInstance runs user code (newTarget.prototype, tag.toString, option getters);
//              it keeps JS values in locals (conservatively scanned) and holds no native borrow across them.
//   Release:   Dispose(bool) is idempotent and only invalidates the magic; there is no native resource.

namespace Js
{
    namespace
    {
        static const size_t NPOS = static_cast<size_t>(-1);

        // Small append-only ASCII buffer: stack storage, heap fallback, sticky OOM flag.
        class TagBuilder
        {
        private:
            char stackBuf[128];
            char* buf;
            size_t len;
            size_t cap;
            bool oom;

        public:
            TagBuilder() : buf(stackBuf), len(0), cap(sizeof(stackBuf)), oom(false)
            {
                stackBuf[0] = '\0';
            }

            ~TagBuilder()
            {
                if (buf != stackBuf)
                {
                    NoCheckHeapDeleteArray(cap, buf);
                }
            }

            TagBuilder(const TagBuilder&) = delete;
            TagBuilder& operator=(const TagBuilder&) = delete;

            bool Append(const char* s, size_t n)
            {
                if (oom)
                {
                    return false;
                }
                if (n == 0)
                {
                    return true;
                }
                if (len + n + 1 > cap)
                {
                    size_t newCap = cap * 2;
                    while (newCap < len + n + 1)
                    {
                        if (newCap > static_cast<size_t>(INT32_MAX) / 2)
                        {
                            oom = true;
                            return false;
                        }
                        newCap *= 2;
                    }
                    char* newBuf = NoCheckHeapNewArray(char, newCap);
                    if (newBuf == nullptr)
                    {
                        oom = true;
                        return false;
                    }
                    js_memcpy_s(newBuf, newCap, buf, len + 1);
                    if (buf != stackBuf)
                    {
                        NoCheckHeapDeleteArray(cap, buf);
                    }
                    buf = newBuf;
                    cap = newCap;
                }
                js_memcpy_s(buf + len, cap - len, s, n);
                len += n;
                buf[len] = '\0';
                return true;
            }

            bool AppendChar(char c) { return Append(&c, 1); }
            bool AppendSz(const char* s) { return Append(s, strlen(s)); }

            // Appends n bytes plus a terminating NUL as one record (used by StringSet).
            bool AppendRecord(const char* s, size_t n)
            {
                if (!Append(s, n))
                {
                    return false;
                }
                return Append("\0", 1);
            }

            void Clear()
            {
                len = 0;
                buf[0] = '\0';
                oom = false;
            }

            const char* Data() const { return buf; }
            size_t Length() const { return len; }
            bool IsOom() const { return oom; }
        };

        // Growable array of plain values with sticky OOM.
        template <typename T>
        class NativeVec
        {
        private:
            T* ptr;
            size_t count;
            size_t cap;
            bool oom;

        public:
            NativeVec() : ptr(nullptr), count(0), cap(0), oom(false) {}
            ~NativeVec()
            {
                if (ptr != nullptr)
                {
                    NoCheckHeapDeleteArray(cap, ptr);
                }
            }
            NativeVec(const NativeVec&) = delete;
            NativeVec& operator=(const NativeVec&) = delete;

            bool Push(const T& item)
            {
                if (oom)
                {
                    return false;
                }
                if (count == cap)
                {
                    size_t newCap = cap == 0 ? 16 : cap * 2;
                    T* newPtr = NoCheckHeapNewArray(T, newCap);
                    if (newPtr == nullptr)
                    {
                        oom = true;
                        return false;
                    }
                    if (count > 0)
                    {
                        js_memcpy_s(newPtr, newCap * sizeof(T), ptr, count * sizeof(T));
                    }
                    if (ptr != nullptr)
                    {
                        NoCheckHeapDeleteArray(cap, ptr);
                    }
                    ptr = newPtr;
                    cap = newCap;
                }
                ptr[count++] = item;
                return true;
            }

            size_t Count() const { return count; }
            bool IsOom() const { return oom; }
            T& operator[](size_t i) { return ptr[i]; }
        };

        // Sorted (code unit order), de-duplicated set of short ASCII strings.
        class StringSet
        {
        private:
            TagBuilder pool;
            NativeVec<size_t> offsets;

        public:
            bool Add(const char* s, size_t n)
            {
                size_t off = pool.Length();
                if (!pool.AppendRecord(s, n) || !offsets.Push(off))
                {
                    return false;
                }
                return true;
            }

            bool IsOom() const { return pool.IsOom() || offsets.IsOom(); }

            // Sorts by strcmp order and removes duplicates; returns the number of unique entries.
            size_t Finish()
            {
                size_t n = offsets.Count();
                const char* base = pool.Data();
                for (size_t i = 1; i < n; ++i)
                {
                    size_t key = offsets[i];
                    size_t j = i;
                    while (j > 0 && strcmp(base + offsets[j - 1], base + key) > 0)
                    {
                        offsets[j] = offsets[j - 1];
                        --j;
                    }
                    offsets[j] = key;
                }
                size_t w = 0;
                for (size_t i = 0; i < n; ++i)
                {
                    if (w == 0 || strcmp(base + offsets[w - 1], base + offsets[i]) != 0)
                    {
                        offsets[w++] = offsets[i];
                    }
                }
                return w;
            }

            const char* At(size_t i) { return pool.Data() + offsets[i]; }
            size_t Count() { return offsets.Count(); }
            // Insertion-order access (valid only before Finish()).
            const char* AtInsertion(size_t i) { return pool.Data() + offsets[i]; }
        };

        // -------------------------------------------------------------------------------------
        // Locale state
        // -------------------------------------------------------------------------------------
        class IntlLocaleState : public IntlModern::IntlModernState
        {
        private:
            Field(JavascriptString*) tag;

        public:
            explicit IntlLocaleState(JavascriptString* localeTag) :
                IntlModern::IntlModernState(IntlModern::IntlModernKind::Locale),
                tag(localeTag)
            {
            }

            virtual JavascriptString* GetLocaleTag() const override
            {
                return tag;
            }

            virtual void Dispose(bool isShutdown) override
            {
                // No native resource is owned; keep the call idempotent.
                InvalidateMagic();
            }

            virtual void Mark(Recycler* recycler) override
            {
                // Not a tracked object: the Field is scanned precisely. Nothing to mark manually.
            }
        };

        IntlLocaleState* RequireLocale(Var thisVar, ScriptContext* scriptContext, const char16* methodName)
        {
            return IntlModern::RequireState<IntlLocaleState>(
                thisVar,
                IntlModern::IntlModernKind::Locale,
                scriptContext,
                methodName);
        }

        // -------------------------------------------------------------------------------------
        // Tag parsing (all inputs are canonical ASCII tags produced by CanonicalizeUnicodeLocaleId)
        // -------------------------------------------------------------------------------------
        struct SubtagIter
        {
            const char* s;
            size_t len;
            size_t pos;

            SubtagIter(const char* str, size_t length) : s(str), len(length), pos(0) {}

            bool Next(size_t& start, size_t& sublen)
            {
                if (pos >= len)
                {
                    return false;
                }
                start = pos;
                size_t i = pos;
                while (i < len && s[i] != '-')
                {
                    ++i;
                }
                sublen = i - start;
                pos = (i < len) ? i + 1 : len;
                return true;
            }
        };

        struct TagInfo
        {
            const char* tag;
            size_t len;
            size_t langStart, langLen;
            size_t scriptStart, scriptLen;
            size_t regionStart, regionLen;
            size_t variantsStart, variantsLen;
            size_t baseLen;
            size_t uStart; // index of the '-' before the 'u' singleton, or NPOS
            size_t uEnd;   // end (exclusive) of the u extension
            size_t xStart; // index of the '-' before the 'x' singleton, or len
        };

        void ParseTag(const char* tag, size_t len, TagInfo& info)
        {
            info.tag = tag;
            info.len = len;
            info.langStart = info.langLen = 0;
            info.scriptStart = info.scriptLen = 0;
            info.regionStart = info.regionLen = 0;
            info.variantsStart = info.variantsLen = 0;
            info.baseLen = len;
            info.uStart = NPOS;
            info.uEnd = len;
            info.xStart = len;

            SubtagIter it(tag, len);
            size_t st = 0, sl = 0;
            if (!it.Next(st, sl))
            {
                return;
            }
            info.langStart = st;
            info.langLen = sl;
            size_t consumedEnd = st + sl;

            SubtagIter peek = it;
            if (peek.Next(st, sl) && IntlModern::IsUnicodeScriptSubtag(tag + st, sl))
            {
                info.scriptStart = st;
                info.scriptLen = sl;
                consumedEnd = st + sl;
                it = peek;
            }
            peek = it;
            if (peek.Next(st, sl) && IntlModern::IsUnicodeRegionSubtag(tag + st, sl))
            {
                info.regionStart = st;
                info.regionLen = sl;
                consumedEnd = st + sl;
                it = peek;
            }
            for (;;)
            {
                peek = it;
                if (!peek.Next(st, sl) || !IntlModern::IsUnicodeVariantSubtag(tag + st, sl))
                {
                    break;
                }
                if (info.variantsLen == 0)
                {
                    info.variantsStart = st;
                }
                consumedEnd = st + sl;
                info.variantsLen = consumedEnd - info.variantsStart;
                it = peek;
            }
            info.baseLen = consumedEnd;

            // Extensions.
            size_t curSingleton = NPOS;
            size_t curStart = 0;
            while (it.Next(st, sl))
            {
                if (sl == 1)
                {
                    if (curSingleton == static_cast<size_t>('u'))
                    {
                        info.uEnd = st - 1;
                    }
                    char c = IntlModern::ToAsciiLower(tag[st]);
                    if (c == 'x')
                    {
                        info.xStart = st - 1;
                        break;
                    }
                    curSingleton = static_cast<size_t>(c);
                    curStart = st - 1;
                    if (c == 'u')
                    {
                        info.uStart = curStart;
                        info.uEnd = len;
                    }
                }
            }
            if (info.uStart != NPOS && info.uEnd > info.xStart)
            {
                info.uEnd = info.xStart;
            }
        }

        // Finds keyword `key` ("ca", ...) in the u extension. Returns true if present; value may be empty.
        bool FindKeyword(const TagInfo& info, const char* key, size_t* outValStart, size_t* outValLen)
        {
            if (info.uStart == NPOS)
            {
                return false;
            }
            SubtagIter it(info.tag, info.uEnd);
            it.pos = info.uStart + 1;
            size_t st = 0, sl = 0;
            it.Next(st, sl); // the 'u' singleton
            bool inKeys = false;
            bool match = false;
            size_t valStart = 0, valEnd = 0;
            bool haveVal = false;
            while (it.Next(st, sl))
            {
                if (sl == 2)
                {
                    if (match)
                    {
                        break;
                    }
                    inKeys = true;
                    match = (info.tag[st] == key[0] && info.tag[st + 1] == key[1]);
                    haveVal = false;
                }
                else if (inKeys && match)
                {
                    if (!haveVal)
                    {
                        valStart = st;
                        haveVal = true;
                    }
                    valEnd = st + sl;
                }
            }
            if (!match)
            {
                return false;
            }
            *outValStart = haveVal ? valStart : 0;
            *outValLen = haveVal ? (valEnd - valStart) : 0;
            return true;
        }

        struct KeywordOverride
        {
            const char* key;
            bool has;
            TagBuilder* value;
        };

        // out = tag with the listed u-extension keywords replaced / added (not yet canonical).
        bool BuildWithKeywords(const TagInfo& info, const KeywordOverride* overrides, size_t overrideCount, TagBuilder& out)
        {
            size_t prefixEnd = (info.uStart != NPOS) ? info.uStart : info.xStart;
            out.Append(info.tag, prefixEnd);
            out.Append("-u", 2);
            if (info.uStart != NPOS)
            {
                SubtagIter it(info.tag, info.uEnd);
                it.pos = info.uStart + 1;
                size_t st = 0, sl = 0;
                it.Next(st, sl); // 'u'
                bool inKeys = false;
                bool skipping = false;
                while (it.Next(st, sl))
                {
                    if (sl == 2)
                    {
                        inKeys = true;
                        skipping = false;
                        for (size_t i = 0; i < overrideCount; ++i)
                        {
                            if (overrides[i].has && info.tag[st] == overrides[i].key[0] && info.tag[st + 1] == overrides[i].key[1])
                            {
                                skipping = true;
                                break;
                            }
                        }
                    }
                    if (!(inKeys && skipping))
                    {
                        out.AppendChar('-');
                        out.Append(info.tag + st, sl);
                    }
                }
            }
            for (size_t i = 0; i < overrideCount; ++i)
            {
                if (overrides[i].has)
                {
                    out.AppendChar('-');
                    out.Append(overrides[i].key, 2);
                    if (overrides[i].value->Length() > 0)
                    {
                        out.AppendChar('-');
                        out.Append(overrides[i].value->Data(), overrides[i].value->Length());
                    }
                }
            }
            size_t suffixStart = (info.uStart != NPOS) ? info.uEnd : info.xStart;
            if (suffixStart < info.len)
            {
                out.Append(info.tag + suffixStart, info.len - suffixStart);
            }
            return !out.IsOom();
        }

        JavascriptString* AsciiToJsString(const char* s, size_t n, ScriptContext* scriptContext)
        {
            return IntlModern::CreateAsciiString(s, n, scriptContext);
        }

        void ThrowIfOom(bool oom, ScriptContext* scriptContext)
        {
            if (oom)
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
        }

        // Copies the (all ASCII) canonical tag out of the state. The returned view is a copy.
        void CopyTag(IntlLocaleState* state, IntlModern::ScopedAsciiString& ascii, ScriptContext* scriptContext)
        {
            JavascriptString* tag = state->GetLocaleTag();
            Assert(tag != nullptr);
            if (!ascii.Init(tag))
            {
                ThrowIfOom(ascii.IsOom(), scriptContext);
                // A non-ASCII canonical tag cannot exist; treat as corrupted state.
                IntlModern::ThrowInvalidReceiverTypeError(scriptContext, _u("Intl.Locale"));
            }
        }

        Var NewLocaleFromTag(ScriptContext* scriptContext, JavascriptString* tag)
        {
            JavascriptLibrary* library = scriptContext->GetLibrary();
            DynamicObject* proto = library->GetIntlLocalePrototype();
            Assert(proto != nullptr);
            DynamicObject* obj = library->CreateObject(proto);
            IntlLocaleState* state = RecyclerNewWithBarrierFinalized(scriptContext->GetRecycler(), IntlLocaleState, tag);
            if (!IntlModern::SetState(obj, state))
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            return obj;
        }

        // -------------------------------------------------------------------------------------
        // Constructor option handling
        // -------------------------------------------------------------------------------------
        struct OptStr
        {
            bool has;
            TagBuilder value;
            OptStr() : has(false) {}
        };

        void CopyOptionAscii(JavascriptString* str, TagBuilder& out, ScriptContext* scriptContext, const char16* errorMessage)
        {
            IntlModern::ScopedAsciiString ascii;
            if (!ascii.Init(str))
            {
                ThrowIfOom(ascii.IsOom(), scriptContext);
                IntlModern::ThrowRangeErrorMessage(scriptContext, errorMessage);
            }
            out.Clear();
            out.Append(ascii.Data(), ascii.Length());
            ThrowIfOom(out.IsOom(), scriptContext);
        }

        // Reads a string option; returns false when undefined. Enumerated values are validated by the shared helper.
        bool ReadStringOption(
            RecyclableObject* options,
            const char16* name,
            const char16* const* values,
            size_t valueCount,
            OptStr& out,
            ScriptContext* scriptContext,
            const char16* errorMessage)
        {
            JavascriptString* str = IntlModern::GetStringOption(options, name, values, valueCount, nullptr, scriptContext);
            if (str == nullptr)
            {
                return false;
            }
            CopyOptionAscii(str, out.value, scriptContext, errorMessage);
            out.has = true;
            return true;
        }

        bool IsAllSubtagsVariants(const char* s, size_t len)
        {
            if (len == 0)
            {
                return false;
            }
            SubtagIter it(s, len);
            size_t st = 0, sl = 0;
            size_t partCount = 0;
            while (it.Next(st, sl))
            {
                if (sl == 0 || !IntlModern::IsUnicodeVariantSubtag(s + st, sl))
                {
                    return false;
                }
                SubtagIter prev(s, len);
                size_t pst = 0, psl = 0;
                while (prev.pos < st && prev.Next(pst, psl))
                {
                    if (psl == sl)
                    {
                        bool same = true;
                        for (size_t i = 0; i < sl; ++i)
                        {
                            if (IntlModern::ToAsciiLower(s[pst + i]) != IntlModern::ToAsciiLower(s[st + i]))
                            {
                                same = false;
                                break;
                            }
                        }
                        if (same)
                        {
                            return false;
                        }
                    }
                }
                ++partCount;
            }
            // Reject empty subtags: a trailing '-' or '--' leaves a gap that SubtagIter would not report.
            if (s[0] == '-' || s[len - 1] == '-')
            {
                return false;
            }
            for (size_t i = 0; i + 1 < len; ++i)
            {
                if (s[i] == '-' && s[i + 1] == '-')
                {
                    return false;
                }
            }
            return partCount > 0;
        }

        const char* WeekdayToString(const char* s, size_t len, size_t* outLen)
        {
            if (len == 1 && s[0] >= '0' && s[0] <= '7')
            {
                static const char* const names[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
                *outLen = 3;
                return names[s[0] - '0'];
            }
            *outLen = len;
            return s;
        }

        JavascriptString* CanonicalizeBuilt(TagBuilder& built, ScriptContext* scriptContext)
        {
            ThrowIfOom(built.IsOom(), scriptContext);
            return IntlModern::CanonicalizeUnicodeLocaleId(built.Data(), built.Length(), scriptContext);
        }

        // -------------------------------------------------------------------------------------
        // ICU lookups
        // -------------------------------------------------------------------------------------
        bool RegionHasSupplementalData(const char* tableKey, const char* region)
        {
            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUResourceBundle sd(ures_openDirect(nullptr, "supplementalData", &status));
            if (U_FAILURE(status) || sd == nullptr)
            {
                return true; // cannot verify; do not reject the region
            }
            PlatformAgnostic::ICUHelpers::ScopedUResourceBundle table(ures_getByKey(sd, tableKey, nullptr, &status));
            if (U_FAILURE(status) || table == nullptr)
            {
                return true;
            }
            PlatformAgnostic::ICUHelpers::ScopedUResourceBundle entry(ures_getByKey(table, region, nullptr, &status));
            return U_SUCCESS(status) && entry != nullptr;
        }

        bool IsAlphaPair(const char* s)
        {
            return IntlModern::IsAsciiAlpha(s[0]) && IntlModern::IsAsciiAlpha(s[1]);
        }

        // RegionPreference: rg keyword (when data exists), region subtag, sd keyword, likely region, "001".
        void ResolveRegionPreference(const TagInfo& info, const char* tableKey, TagBuilder& out, ScriptContext* scriptContext)
        {
            out.Clear();

            size_t vs = 0, vl = 0;
            if (FindKeyword(info, "rg", &vs, &vl) && vl >= 2 && IsAlphaPair(info.tag + vs))
            {
                char r[3] = { IntlModern::ToAsciiUpper(info.tag[vs]), IntlModern::ToAsciiUpper(info.tag[vs + 1]), '\0' };
                if (RegionHasSupplementalData(tableKey, r))
                {
                    out.Append(r, 2);
                    return;
                }
            }

            if (info.regionLen > 0)
            {
                for (size_t i = 0; i < info.regionLen; ++i)
                {
                    out.AppendChar(IntlModern::ToAsciiUpper(info.tag[info.regionStart + i]));
                }
                return;
            }

            if (FindKeyword(info, "sd", &vs, &vl) && vl >= 2 && IsAlphaPair(info.tag + vs))
            {
                out.AppendChar(IntlModern::ToAsciiUpper(info.tag[vs]));
                out.AppendChar(IntlModern::ToAsciiUpper(info.tag[vs + 1]));
                return;
            }

            JavascriptString* maxTag = IntlModern::MaximizeLocaleId(info.tag, info.baseLen, scriptContext);
            IntlModern::ScopedAsciiString maxAscii;
            if (maxTag != nullptr && maxAscii.Init(maxTag))
            {
                TagInfo maxInfo;
                ParseTag(maxAscii.Data(), maxAscii.Length(), maxInfo);
                if (maxInfo.regionLen > 0)
                {
                    for (size_t i = 0; i < maxInfo.regionLen; ++i)
                    {
                        out.AppendChar(IntlModern::ToAsciiUpper(maxInfo.tag[maxInfo.regionStart + i]));
                    }
                    ThrowIfOom(out.IsOom(), scriptContext);
                    return;
                }
            }
            out.AppendSz("001");
            ThrowIfOom(out.IsOom(), scriptContext);
        }

        // ICU locale id "lang[_Script][_REGION]" from the tag's base name parts.
        void BuildIcuLocaleId(const TagInfo& info, const char* regionOverride, bool includeScript, TagBuilder& out)
        {
            out.Clear();
            out.Append(info.tag + info.langStart, info.langLen);
            if (includeScript && info.scriptLen > 0)
            {
                out.AppendChar('_');
                out.Append(info.tag + info.scriptStart, info.scriptLen);
            }
            if (regionOverride != nullptr)
            {
                out.AppendChar('_');
                out.AppendSz(regionOverride);
            }
            else if (info.regionLen > 0)
            {
                out.AppendChar('_');
                out.Append(info.tag + info.regionStart, info.regionLen);
            }
        }

        JavascriptArray* StringSetToArray(StringSet& set, size_t count, ScriptContext* scriptContext)
        {
            JavascriptArray* result = scriptContext->GetLibrary()->CreateArray(static_cast<uint32>(count));
            for (size_t i = 0; i < count; ++i)
            {
                const char* s = set.At(i);
                result->DirectSetItemAt(static_cast<uint32>(i), AsciiToJsString(s, strlen(s), scriptContext));
            }
            return result;
        }

        JavascriptArray* SingleStringArray(const char* s, size_t n, ScriptContext* scriptContext)
        {
            JavascriptArray* result = scriptContext->GetLibrary()->CreateArray(1);
            result->DirectSetItemAt(0, AsciiToJsString(s, n, scriptContext));
            return result;
        }

        // Converts a legacy ICU keyword value to its BCP 47 type and appends it to the set.
        bool AddKeywordTypeToSet(const char* bcpKey, const char* icuValue, StringSet& set, const char* const* exclude, size_t excludeCount)
        {
            const char* type = uloc_toUnicodeLocaleType(bcpKey, icuValue);
            if (type == nullptr || type[0] == '\0')
            {
                type = icuValue;
            }
            TagBuilder lowered;
            for (const char* p = type; *p != '\0'; ++p)
            {
                lowered.AppendChar(IntlModern::ToAsciiLower(*p));
            }
            for (size_t i = 0; i < excludeCount; ++i)
            {
                if (strcmp(lowered.Data(), exclude[i]) == 0)
                {
                    return true;
                }
            }
            return set.Add(lowered.Data(), lowered.Length());
        }

        const char* ExtractKeywordOrNull(const TagInfo& info, const char* key, size_t* outLen)
        {
            size_t vs = 0, vl = 0;
            if (!FindKeyword(info, key, &vs, &vl))
            {
                return nullptr;
            }
            *outLen = vl;
            return info.tag + vs;
        }

        // Shared getter plumbing: parse the receiver's tag then run `fn`.
        template <typename Fn>
        Var WithLocaleInfo(RecyclableObject* function, CallInfo callInfo, Var thisVar, const char16* methodName, Fn fn)
        {
            ScriptContext* scriptContext = function->GetScriptContext();
            IntlLocaleState* state = RequireLocale(thisVar, scriptContext, methodName);
            IntlModern::ScopedAsciiString ascii;
            CopyTag(state, ascii, scriptContext);
            TagInfo info;
            ParseTag(ascii.Data(), ascii.Length(), info);
            return fn(scriptContext, info);
        }

        Var KeywordGetter(const TagInfo& info, const char* key, ScriptContext* scriptContext)
        {
            size_t len = 0;
            const char* v = ExtractKeywordOrNull(info, key, &len);
            if (v == nullptr)
            {
                return scriptContext->GetLibrary()->GetUndefined();
            }
            return AsciiToJsString(v, len, scriptContext);
        }

        Var SubtagGetter(const TagInfo& info, size_t start, size_t len, ScriptContext* scriptContext)
        {
            if (len == 0)
            {
                return scriptContext->GetLibrary()->GetUndefined();
            }
            return AsciiToJsString(info.tag + start, len, scriptContext);
        }

        Var GetThisArg(Arguments& args, ScriptContext* scriptContext)
        {
            return args.Info.Count > 0 ? args[0] : scriptContext->GetLibrary()->GetUndefined();
        }
    } // namespace

    void IntlLocale::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        DynamicObject* proto = IntlModern::RegisterIntlClass(
            library,
            intlObject,
            _u("Locale"),
            &EntryInfo::NewInstance,
            1,
            _u("Intl.Locale"));

        library->SetIntlLocalePrototype(proto);

        library->AddFunctionToLibraryObject(proto, PropertyIds::toString, &EntryInfo::ToString, 0);
        IntlModern::AddIntlMethod(library, proto, _u("maximize"), &EntryInfo::Maximize, 0);
        IntlModern::AddIntlMethod(library, proto, _u("minimize"), &EntryInfo::Minimize, 0);

        IntlModern::AddIntlGetter(library, proto, _u("baseName"), &EntryInfo::GetterBaseName);
        library->AddGetterToLibraryObject(proto, PropertyIds::calendar, &EntryInfo::GetterCalendar);
        IntlModern::AddIntlGetter(library, proto, _u("caseFirst"), &EntryInfo::GetterCaseFirst);
        IntlModern::AddIntlGetter(library, proto, _u("collation"), &EntryInfo::GetterCollation);
        IntlModern::AddIntlGetter(library, proto, _u("firstDayOfWeek"), &EntryInfo::GetterFirstDayOfWeek);
        IntlModern::AddIntlGetter(library, proto, _u("hourCycle"), &EntryInfo::GetterHourCycle);
        IntlModern::AddIntlGetter(library, proto, _u("language"), &EntryInfo::GetterLanguage);
        library->AddGetterToLibraryObject(proto, PropertyIds::numberingSystem, &EntryInfo::GetterNumberingSystem);
        library->AddGetterToLibraryObject(proto, PropertyIds::numeric, &EntryInfo::GetterNumeric);
        IntlModern::AddIntlGetter(library, proto, _u("region"), &EntryInfo::GetterRegion);
        IntlModern::AddIntlGetter(library, proto, _u("script"), &EntryInfo::GetterScript);
        IntlModern::AddIntlGetter(library, proto, _u("variants"), &EntryInfo::GetterVariants);

        IntlModern::AddIntlMethod(library, proto, _u("getCalendars"), &EntryInfo::GetCalendars, 0);
        IntlModern::AddIntlMethod(library, proto, _u("getCollations"), &EntryInfo::GetCollations, 0);
        IntlModern::AddIntlMethod(library, proto, _u("getHourCycles"), &EntryInfo::GetHourCycles, 0);
        IntlModern::AddIntlMethod(library, proto, _u("getNumberingSystems"), &EntryInfo::GetNumberingSystems, 0);
        IntlModern::AddIntlMethod(library, proto, _u("getTimeZones"), &EntryInfo::GetTimeZones, 0);
        IntlModern::AddIntlMethod(library, proto, _u("getTextInfo"), &EntryInfo::GetTextInfo, 0);
        IntlModern::AddIntlMethod(library, proto, _u("getWeekInfo"), &EntryInfo::GetWeekInfo, 0);
    }

    Var IntlLocale::EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.Locale"));
        }
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.Locale"));
        }

        Var undefinedVar = scriptContext->GetLibrary()->GetUndefined();
        Var tagArg = args.Info.Count > 1 ? args[1] : undefinedVar;
        Var optionsArg = args.Info.Count > 2 ? args[2] : undefinedVar;

        // Steps: OrdinaryCreateFromConstructor, then the tag/options coercions (all may run user code).
        DynamicObject* obj = IntlModern::OrdinaryCreateFromConstructor(newTarget, scriptContext, &JavascriptLibrary::GetIntlLocalePrototype);

        if (!JavascriptOperators::IsObject(tagArg) && !VarIs<JavascriptString>(tagArg))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }

        JavascriptString* tagString = nullptr;
        if (JavascriptOperators::IsObject(tagArg))
        {
            tagString = IntlModern::TryGetLocaleTagIfLocaleObject(tagArg);
        }
        if (tagString == nullptr)
        {
            tagString = JavascriptConversion::ToString(tagArg, scriptContext);
        }

        RecyclableObject* options = IntlModern::CoerceOptionsToObject(optionsArg, scriptContext);

        // Structural validation + first canonicalization (RangeError for an invalid tag).
        JavascriptString* canonical = IntlModern::ValidateAndCanonicalizeUnicodeLocaleId(tagString, scriptContext);

        // ---- ApplyOptionsToTag: language, script, region, variants (observable order) ----
        OptStr language, script, region, variants;
        ReadStringOption(options, _u("language"), nullptr, 0, language, scriptContext, _u("Invalid language option"));
        if (language.has && !IntlModern::IsUnicodeLanguageSubtag(language.value.Data(), language.value.Length()))
        {
            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Invalid language option"));
        }
        ReadStringOption(options, _u("script"), nullptr, 0, script, scriptContext, _u("Invalid script option"));
        if (script.has && !IntlModern::IsUnicodeScriptSubtag(script.value.Data(), script.value.Length()))
        {
            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Invalid script option"));
        }
        ReadStringOption(options, _u("region"), nullptr, 0, region, scriptContext, _u("Invalid region option"));
        if (region.has && !IntlModern::IsUnicodeRegionSubtag(region.value.Data(), region.value.Length()))
        {
            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Invalid region option"));
        }
        ReadStringOption(options, _u("variants"), nullptr, 0, variants, scriptContext, _u("Invalid variants option"));
        if (variants.has && !IsAllSubtagsVariants(variants.value.Data(), variants.value.Length()))
        {
            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Invalid variants option"));
        }

        JavascriptString* tagAfterLang = canonical;
        if (language.has || script.has || region.has || variants.has)
        {
            IntlModern::ScopedAsciiString canonAscii;
            if (!canonAscii.Init(canonical))
            {
                ThrowIfOom(canonAscii.IsOom(), scriptContext);
                IntlModern::ThrowLocaleRangeError(scriptContext, canonical);
            }
            TagInfo ci;
            ParseTag(canonAscii.Data(), canonAscii.Length(), ci);

            TagBuilder built;
            if (language.has)
            {
                built.Append(language.value.Data(), language.value.Length());
            }
            else
            {
                built.Append(ci.tag + ci.langStart, ci.langLen);
            }
            if (script.has)
            {
                built.AppendChar('-');
                built.Append(script.value.Data(), script.value.Length());
            }
            else if (ci.scriptLen > 0)
            {
                built.AppendChar('-');
                built.Append(ci.tag + ci.scriptStart, ci.scriptLen);
            }
            if (region.has)
            {
                built.AppendChar('-');
                built.Append(region.value.Data(), region.value.Length());
            }
            else if (ci.regionLen > 0)
            {
                built.AppendChar('-');
                built.Append(ci.tag + ci.regionStart, ci.regionLen);
            }
            if (variants.has)
            {
                built.AppendChar('-');
                built.Append(variants.value.Data(), variants.value.Length());
            }
            else if (ci.variantsLen > 0)
            {
                built.AppendChar('-');
                built.Append(ci.tag + ci.variantsStart, ci.variantsLen);
            }
            if (ci.baseLen < ci.len)
            {
                built.Append(ci.tag + ci.baseLen, ci.len - ci.baseLen);
            }
            tagAfterLang = CanonicalizeBuilt(built, scriptContext);
        }

        // ---- Unicode extension keyword options (observable order) ----
        static const char16* const hourCycleValues[] = { _u("h11"), _u("h12"), _u("h23"), _u("h24") };
        static const char16* const caseFirstValues[] = { _u("upper"), _u("lower"), _u("false") };

        OptStr ca, co, fw, hc, kf, kn, nu;
        if (ReadStringOption(options, _u("calendar"), nullptr, 0, ca, scriptContext, _u("Invalid calendar option")) &&
            !IntlModern::IsUnicodeTypeNonterminal(ca.value.Data(), ca.value.Length()))
        {
            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Invalid calendar option"));
        }
        if (ReadStringOption(options, _u("collation"), nullptr, 0, co, scriptContext, _u("Invalid collation option")) &&
            !IntlModern::IsUnicodeTypeNonterminal(co.value.Data(), co.value.Length()))
        {
            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Invalid collation option"));
        }
        if (ReadStringOption(options, _u("firstDayOfWeek"), nullptr, 0, fw, scriptContext, _u("Invalid firstDayOfWeek option")))
        {
            size_t mappedLen = 0;
            const char* mapped = WeekdayToString(fw.value.Data(), fw.value.Length(), &mappedLen);
            if (!IntlModern::IsUnicodeTypeNonterminal(mapped, mappedLen))
            {
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Invalid firstDayOfWeek option"));
            }
            TagBuilder mappedCopy;
            mappedCopy.Append(mapped, mappedLen);
            fw.value.Clear();
            fw.value.Append(mappedCopy.Data(), mappedCopy.Length());
        }
        ReadStringOption(options, _u("hourCycle"), hourCycleValues, _countof(hourCycleValues), hc, scriptContext, _u("Invalid hourCycle option"));
        ReadStringOption(options, _u("caseFirst"), caseFirstValues, _countof(caseFirstValues), kf, scriptContext, _u("Invalid caseFirst option"));
        int numericOpt = IntlModern::GetBoolOption(options, _u("numeric"), -1, scriptContext);
        if (numericOpt >= 0)
        {
            kn.has = true;
            kn.value.AppendSz(numericOpt != 0 ? "true" : "false");
        }
        if (ReadStringOption(options, _u("numberingSystem"), nullptr, 0, nu, scriptContext, _u("Invalid numberingSystem option")) &&
            !IntlModern::IsUnicodeTypeNonterminal(nu.value.Data(), nu.value.Length()))
        {
            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Invalid numberingSystem option"));
        }

        JavascriptString* finalTag = tagAfterLang;
        if (ca.has || co.has || fw.has || hc.has || kf.has || kn.has || nu.has)
        {
            IntlModern::ScopedAsciiString langAscii;
            if (!langAscii.Init(tagAfterLang))
            {
                ThrowIfOom(langAscii.IsOom(), scriptContext);
                IntlModern::ThrowLocaleRangeError(scriptContext, tagAfterLang);
            }
            TagInfo li;
            ParseTag(langAscii.Data(), langAscii.Length(), li);
            KeywordOverride overrides[] = {
                { "ca", ca.has, &ca.value },
                { "co", co.has, &co.value },
                { "fw", fw.has, &fw.value },
                { "hc", hc.has, &hc.value },
                { "kf", kf.has, &kf.value },
                { "kn", kn.has, &kn.value },
                { "nu", nu.has, &nu.value },
            };
            TagBuilder built;
            BuildWithKeywords(li, overrides, _countof(overrides), built);
            finalTag = CanonicalizeBuilt(built, scriptContext);
        }

        IntlLocaleState* state = RecyclerNewWithBarrierFinalized(scriptContext->GetRecycler(), IntlLocaleState, finalTag);
        if (!IntlModern::SetState(obj, state))
        {
            JavascriptError::ThrowOutOfMemoryError(scriptContext);
        }
        return obj;
    }

    Var IntlLocale::EntryToString(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        IntlLocaleState* state = RequireLocale(GetThisArg(args, scriptContext), scriptContext, _u("Intl.Locale.prototype.toString"));
        return state->GetLocaleTag();
    }

    Var IntlLocale::EntryMaximize(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        return WithLocaleInfo(function, callInfo, GetThisArg(args, scriptContext), _u("Intl.Locale.prototype.maximize"),
            [](ScriptContext* ctx, const TagInfo& info) -> Var
            {
                JavascriptString* result = IntlModern::MaximizeLocaleId(info.tag, info.len, ctx);
                return NewLocaleFromTag(ctx, result);
            });
    }

    Var IntlLocale::EntryMinimize(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        return WithLocaleInfo(function, callInfo, GetThisArg(args, scriptContext), _u("Intl.Locale.prototype.minimize"),
            [](ScriptContext* ctx, const TagInfo& info) -> Var
            {
                JavascriptString* result = IntlModern::MinimizeLocaleId(info.tag, info.len, ctx);
                return NewLocaleFromTag(ctx, result);
            });
    }

#define INTL_LOCALE_GETTER(Name, DisplayName, ...) \
    Var IntlLocale::Name(RecyclableObject* function, CallInfo callInfo, ...) \
    { \
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault); \
        ARGUMENTS(args, callInfo); \
        ScriptContext* scriptContext = function->GetScriptContext(); \
        return WithLocaleInfo(function, callInfo, GetThisArg(args, scriptContext), DisplayName, \
            [](ScriptContext* ctx, const TagInfo& info) -> Var { __VA_ARGS__ }); \
    }

    INTL_LOCALE_GETTER(EntryGetterBaseName, _u("get Intl.Locale.prototype.baseName"),
        return AsciiToJsString(info.tag, info.baseLen, ctx);)
    INTL_LOCALE_GETTER(EntryGetterCalendar, _u("get Intl.Locale.prototype.calendar"),
        return KeywordGetter(info, "ca", ctx);)
    INTL_LOCALE_GETTER(EntryGetterCaseFirst, _u("get Intl.Locale.prototype.caseFirst"),
        return KeywordGetter(info, "kf", ctx);)
    INTL_LOCALE_GETTER(EntryGetterCollation, _u("get Intl.Locale.prototype.collation"),
        return KeywordGetter(info, "co", ctx);)
    INTL_LOCALE_GETTER(EntryGetterFirstDayOfWeek, _u("get Intl.Locale.prototype.firstDayOfWeek"),
        return KeywordGetter(info, "fw", ctx);)
    INTL_LOCALE_GETTER(EntryGetterHourCycle, _u("get Intl.Locale.prototype.hourCycle"),
        return KeywordGetter(info, "hc", ctx);)
    INTL_LOCALE_GETTER(EntryGetterLanguage, _u("get Intl.Locale.prototype.language"),
        return AsciiToJsString(info.tag + info.langStart, info.langLen, ctx);)
    INTL_LOCALE_GETTER(EntryGetterNumberingSystem, _u("get Intl.Locale.prototype.numberingSystem"),
        return KeywordGetter(info, "nu", ctx);)
    INTL_LOCALE_GETTER(EntryGetterNumeric, _u("get Intl.Locale.prototype.numeric"),
        {
            size_t len = 0;
            const char* v = ExtractKeywordOrNull(info, "kn", &len);
            bool numeric = v != nullptr && (len == 0 || (len == 4 && strncmp(v, "true", 4) == 0));
            JavascriptLibrary* library = ctx->GetLibrary();
            return numeric ? static_cast<Var>(library->GetTrue()) : static_cast<Var>(library->GetFalse());
        })
    INTL_LOCALE_GETTER(EntryGetterRegion, _u("get Intl.Locale.prototype.region"),
        return SubtagGetter(info, info.regionStart, info.regionLen, ctx);)
    INTL_LOCALE_GETTER(EntryGetterScript, _u("get Intl.Locale.prototype.script"),
        return SubtagGetter(info, info.scriptStart, info.scriptLen, ctx);)
    INTL_LOCALE_GETTER(EntryGetterVariants, _u("get Intl.Locale.prototype.variants"),
        return SubtagGetter(info, info.variantsStart, info.variantsLen, ctx);)

    INTL_LOCALE_GETTER(EntryGetCalendars, _u("Intl.Locale.prototype.getCalendars"),
        {
            size_t len = 0;
            const char* preferred = ExtractKeywordOrNull(info, "ca", &len);
            if (preferred != nullptr)
            {
                return SingleStringArray(preferred, len, ctx);
            }

            TagBuilder region;
            ResolveRegionPreference(info, "calendarPreferenceData", region, ctx);
            TagBuilder icuId;
            icuId.AppendSz("und_");
            icuId.Append(region.Data(), region.Length());
            ThrowIfOom(icuId.IsOom(), ctx);

            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUEnumeration values(
                ucal_getKeywordValuesForLocale("calendar", icuId.Data(), TRUE, &status));
            StringSet set;
            if (U_SUCCESS(status) && values != nullptr)
            {
                // Preference order is meaningful, so keep ICU's order (de-duplicated, not sorted).
                for (;;)
                {
                    int32_t vlen = 0;
                    const char* v = uenum_next(values, &vlen, &status);
                    if (v == nullptr || U_FAILURE(status))
                    {
                        break;
                    }
                    const char* type = uloc_toUnicodeLocaleType("ca", v);
                    if (type == nullptr || type[0] == '\0')
                    {
                        type = v;
                    }
                    TagBuilder lowered;
                    for (const char* p = type; *p != '\0'; ++p)
                    {
                        lowered.AppendChar(IntlModern::ToAsciiLower(*p));
                    }
                    set.Add(lowered.Data(), lowered.Length());
                }
            }
            ThrowIfOom(set.IsOom(), ctx);
            // Order-preserving unique output: iterate the pool in insertion order.
            size_t n = set.Count();
            JavascriptArray* result = ctx->GetLibrary()->CreateArray(0);
            uint32 outCount = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const char* s = set.AtInsertion(i);
                bool dup = false;
                for (size_t j = 0; j < i; ++j)
                {
                    if (strcmp(set.AtInsertion(j), s) == 0)
                    {
                        dup = true;
                        break;
                    }
                }
                if (!dup)
                {
                    result->DirectSetItemAt(outCount++, AsciiToJsString(s, strlen(s), ctx));
                }
            }
            if (outCount == 0)
            {
                result->DirectSetItemAt(0, AsciiToJsString("gregory", 7, ctx));
            }
            return result;
        })

    INTL_LOCALE_GETTER(EntryGetCollations, _u("Intl.Locale.prototype.getCollations"),
        {
            size_t len = 0;
            const char* preferred = ExtractKeywordOrNull(info, "co", &len);
            if (preferred != nullptr)
            {
                return SingleStringArray(preferred, len, ctx);
            }

            TagBuilder icuId;
            BuildIcuLocaleId(info, nullptr, true, icuId);
            ThrowIfOom(icuId.IsOom(), ctx);

            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUEnumeration values(
                ucol_getKeywordValuesForLocale("collation", icuId.Data(), FALSE, &status));
            StringSet set;
            static const char* const excluded[] = { "standard", "search" };
            if (U_SUCCESS(status) && values != nullptr)
            {
                for (;;)
                {
                    int32_t vlen = 0;
                    const char* v = uenum_next(values, &vlen, &status);
                    if (v == nullptr || U_FAILURE(status))
                    {
                        break;
                    }
                    AddKeywordTypeToSet("co", v, set, excluded, _countof(excluded));
                }
            }
            ThrowIfOom(set.IsOom(), ctx);
            size_t n = set.Finish();
            if (n == 0)
            {
                // Hardcoded root collations.
                set.Add("emoji", 5);
                set.Add("eor", 3);
                ThrowIfOom(set.IsOom(), ctx);
                n = set.Finish();
            }
            return StringSetToArray(set, n, ctx);
        })

    INTL_LOCALE_GETTER(EntryGetHourCycles, _u("Intl.Locale.prototype.getHourCycles"),
        {
            size_t len = 0;
            const char* preferred = ExtractKeywordOrNull(info, "hc", &len);
            if (preferred != nullptr)
            {
                return SingleStringArray(preferred, len, ctx);
            }

            TagBuilder region;
            ResolveRegionPreference(info, "timeData", region, ctx);
            TagBuilder icuId;
            icuId.Append(info.tag + info.langStart, info.langLen);
            icuId.AppendChar('_');
            icuId.Append(region.Data(), region.Length());
            ThrowIfOom(icuId.IsOom(), ctx);

            const char* hourCycle = "h23";
            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUDateTimePatternGenerator dtpg(udatpg_open(icuId.Data(), &status));
            if (U_SUCCESS(status) && dtpg != nullptr)
            {
                UDateFormatHourCycle hc = udatpg_getDefaultHourCycle(dtpg, &status);
                if (U_SUCCESS(status))
                {
                    switch (hc)
                    {
                    case UDAT_HOUR_CYCLE_11: hourCycle = "h11"; break;
                    case UDAT_HOUR_CYCLE_12: hourCycle = "h12"; break;
                    case UDAT_HOUR_CYCLE_23: hourCycle = "h23"; break;
                    case UDAT_HOUR_CYCLE_24: hourCycle = "h24"; break;
                    default: break;
                    }
                }
            }
            return SingleStringArray(hourCycle, 3, ctx);
        })

    INTL_LOCALE_GETTER(EntryGetNumberingSystems, _u("Intl.Locale.prototype.getNumberingSystems"),
        {
            size_t len = 0;
            const char* preferred = ExtractKeywordOrNull(info, "nu", &len);
            if (preferred != nullptr)
            {
                return SingleStringArray(preferred, len, ctx);
            }

            TagBuilder icuId;
            BuildIcuLocaleId(info, nullptr, true, icuId);
            ThrowIfOom(icuId.IsOom(), ctx);

            const char* system = "latn";
            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUNumberingSystem ns(unumsys_open(icuId.Data(), &status));
            TagBuilder name;
            if (U_SUCCESS(status) && ns != nullptr && !unumsys_isAlgorithmic(ns))
            {
                const char* n = unumsys_getName(ns);
                if (n != nullptr && n[0] != '\0')
                {
                    name.AppendSz(n);
                    ThrowIfOom(name.IsOom(), ctx);
                    system = name.Data();
                }
            }
            return SingleStringArray(system, strlen(system), ctx);
        })

    INTL_LOCALE_GETTER(EntryGetTimeZones, _u("Intl.Locale.prototype.getTimeZones"),
        {
            if (info.regionLen == 0)
            {
                return ctx->GetLibrary()->GetUndefined();
            }
            TagBuilder region;
            for (size_t i = 0; i < info.regionLen; ++i)
            {
                region.AppendChar(IntlModern::ToAsciiUpper(info.tag[info.regionStart + i]));
            }
            ThrowIfOom(region.IsOom(), ctx);

            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUEnumeration zones(
                ucal_openTimeZoneIDEnumeration(UCAL_ZONE_TYPE_CANONICAL_LOCATION, region.Data(), nullptr, &status));
            StringSet set;
            if (U_SUCCESS(status) && zones != nullptr)
            {
                for (;;)
                {
                    int32_t zlen = 0;
                    const char* z = uenum_next(zones, &zlen, &status);
                    if (z == nullptr || U_FAILURE(status))
                    {
                        break;
                    }
                    set.Add(z, static_cast<size_t>(zlen));
                }
            }
            ThrowIfOom(set.IsOom(), ctx);
            size_t n = set.Finish();
            return StringSetToArray(set, n, ctx);
        })

    INTL_LOCALE_GETTER(EntryGetTextInfo, _u("Intl.Locale.prototype.getTextInfo"),
        {
            TagBuilder scriptName;
            if (info.scriptLen > 0)
            {
                scriptName.Append(info.tag + info.scriptStart, info.scriptLen);
            }
            else
            {
                JavascriptString* maxTag = IntlModern::MaximizeLocaleId(info.tag, info.baseLen, ctx);
                IntlModern::ScopedAsciiString maxAscii;
                if (maxTag != nullptr && maxAscii.Init(maxTag))
                {
                    TagInfo maxInfo;
                    ParseTag(maxAscii.Data(), maxAscii.Length(), maxInfo);
                    if (maxInfo.scriptLen > 0)
                    {
                        scriptName.Append(maxInfo.tag + maxInfo.scriptStart, maxInfo.scriptLen);
                    }
                }
            }
            ThrowIfOom(scriptName.IsOom(), ctx);

            const char* direction = nullptr;
            if (scriptName.Length() > 0)
            {
                int32_t code = u_getPropertyValueEnum(UCHAR_SCRIPT, scriptName.Data());
                if (code != UCHAR_INVALID_CODE &&
                    code != USCRIPT_COMMON && code != USCRIPT_INHERITED &&
                    code != USCRIPT_UNKNOWN && code != USCRIPT_BRAILLE &&
                    code != USCRIPT_UNWRITTEN_LANGUAGES && code != USCRIPT_SYMBOLS &&
                    code != USCRIPT_SYMBOLS_EMOJI && code != USCRIPT_MATHEMATICAL_NOTATION)
                {
                    UScriptCode scriptCode = static_cast<UScriptCode>(code);
                    if (uscript_isRightToLeft(scriptCode))
                    {
                        direction = "rtl";
                    }
                    else
                    {
                        // Classify from the script's sample character so scripts ICU does not list as
                        // right-to-left (e.g. Aran) are never reported as "ltr" by default.
                        UChar sample[8] = { 0 };
                        UErrorCode sampleStatus = U_ZERO_ERROR;
                        int32_t sampleLen = uscript_getSampleString(scriptCode, sample, static_cast<int32_t>(_countof(sample)), &sampleStatus);
                        if (U_SUCCESS(sampleStatus) && sampleLen > 0)
                        {
                            UChar32 first = 0;
                            U16_GET(sample, 0, 0, sampleLen, first);
                            switch (u_charDirection(first))
                            {
                            case U_LEFT_TO_RIGHT:
                                direction = "ltr";
                                break;
                            case U_RIGHT_TO_LEFT:
                            case U_RIGHT_TO_LEFT_ARABIC:
                                direction = "rtl";
                                break;
                            default:
                                break;
                            }
                        }
                    }
                }
            }

            DynamicObject* result = IntlModern::CreatePlainDataObject(ctx);
            Var dirVar = direction != nullptr
                ? static_cast<Var>(AsciiToJsString(direction, 3, ctx))
                : ctx->GetLibrary()->GetUndefined();
            IntlModern::SetDataProperty(result, _u("direction"), dirVar, ctx);
            return result;
        })

    INTL_LOCALE_GETTER(EntryGetWeekInfo, _u("Intl.Locale.prototype.getWeekInfo"),
        {
            TagBuilder region;
            ResolveRegionPreference(info, "weekData", region, ctx);
            TagBuilder icuId;
            icuId.AppendSz("und_");
            icuId.Append(region.Data(), region.Length());
            ThrowIfOom(icuId.IsOom(), ctx);

            // ICU numbers days 1 (Sunday) .. 7 (Saturday); ECMA-402 uses 1 (Monday) .. 7 (Sunday).
            int firstDay = 1;
            int weekend[7] = { 0 };
            int weekendCount = 0;
            UErrorCode status = U_ZERO_ERROR;
            static const UChar utc[] = { 'U', 'T', 'C' };
            UCalendar* rawCal = ucal_open(utc, 3, icuId.Data(), UCAL_GREGORIAN, &status);
            if (U_SUCCESS(status) && rawCal != nullptr)
            {
                int32_t icuFirst = ucal_getAttribute(rawCal, UCAL_FIRST_DAY_OF_WEEK);
                firstDay = (icuFirst == 1) ? 7 : (icuFirst - 1);
                for (int32_t dow = 1; dow <= 7; ++dow)
                {
                    UErrorCode dstatus = U_ZERO_ERROR;
                    UCalendarWeekdayType type = ucal_getDayOfWeekType(rawCal, static_cast<UCalendarDaysOfWeek>(dow), &dstatus);
                    if (U_SUCCESS(dstatus) && type != UCAL_WEEKDAY)
                    {
                        weekend[weekendCount++] = (dow == 1) ? 7 : (dow - 1);
                    }
                }
            }
            if (rawCal != nullptr)
            {
                ucal_close(rawCal);
            }

            size_t fwLen = 0;
            const char* fwValue = ExtractKeywordOrNull(info, "fw", &fwLen);
            if (fwValue != nullptr && fwLen == 3)
            {
                static const char* const names[] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
                for (int i = 0; i < 7; ++i)
                {
                    if (strncmp(fwValue, names[i], 3) == 0)
                    {
                        firstDay = i + 1;
                        break;
                    }
                }
            }

            // Ascending order.
            for (int i = 1; i < weekendCount; ++i)
            {
                int key = weekend[i];
                int j = i;
                while (j > 0 && weekend[j - 1] > key)
                {
                    weekend[j] = weekend[j - 1];
                    --j;
                }
                weekend[j] = key;
            }

            JavascriptArray* weekendArray = ctx->GetLibrary()->CreateArray(static_cast<uint32>(weekendCount));
            for (int i = 0; i < weekendCount; ++i)
            {
                weekendArray->DirectSetItemAt(static_cast<uint32>(i), TaggedInt::ToVarUnchecked(weekend[i]));
            }
            DynamicObject* result = IntlModern::CreatePlainDataObject(ctx);
            IntlModern::SetDataProperty(result, _u("firstDay"), TaggedInt::ToVarUnchecked(firstDay), ctx);
            IntlModern::SetDataProperty(result, _u("weekend"), weekendArray, ctx);
            return result;
        })

#undef INTL_LOCALE_GETTER
} // namespace Js

#endif // INTL_ICU
