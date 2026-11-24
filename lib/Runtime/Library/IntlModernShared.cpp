//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"

#ifdef INTL_ICU

namespace Js
{
    namespace IntlModern
    {
        namespace
        {
            static const int gIntlModernStateSentinel = 0;

            struct Subtag
            {
                size_t start;
                size_t len;
            };

            template <typename T, size_t StackCap>
            class SmallList
            {
            private:
                T stack[StackCap];
                T* ptr;
                size_t count;
                size_t cap;
                bool oom;

            public:
                SmallList() : ptr(stack), count(0), cap(StackCap), oom(false) {}

                ~SmallList()
                {
                    if (ptr != stack)
                    {
                        NoCheckHeapDeleteArray(cap, ptr);
                    }
                }

                bool Push(const T& item)
                {
                    if (oom)
                    {
                        return false;
                    }
                    if (count == cap)
                    {
                        size_t newCap = cap * 2;
                        T* newPtr = NoCheckHeapNewArray(T, newCap);
                        if (newPtr == nullptr)
                        {
                            oom = true;
                            return false;
                        }
                        js_memcpy_s(newPtr, newCap * sizeof(T), ptr, count * sizeof(T));
                        if (ptr != stack)
                        {
                            NoCheckHeapDeleteArray(cap, ptr);
                        }
                        ptr = newPtr;
                        cap = newCap;
                    }
                    ptr[count++] = item;
                    return true;
                }

                void RemoveAt(size_t index)
                {
                    if (index >= count)
                    {
                        return;
                    }
                    for (size_t i = index + 1; i < count; ++i)
                    {
                        ptr[i - 1] = ptr[i];
                    }
                    --count;
                }

                void Clear() { count = 0; }
                size_t Count() const { return count; }
                bool IsOom() const { return oom; }
                T& operator[](size_t index) { return ptr[index]; }
                const T& operator[](size_t index) const { return ptr[index]; }
            };

            class CharBuffer
            {
            private:
                char stackBuf[256];
                char* buf;
                size_t len;
                size_t cap;
                bool oom;

            public:
                CharBuffer() : buf(stackBuf), len(0), cap(sizeof(stackBuf)), oom(false)
                {
                    stackBuf[0] = '\0';
                }

                ~CharBuffer()
                {
                    if (buf != stackBuf)
                    {
                        NoCheckHeapDeleteArray(cap, buf);
                    }
                }

                bool EnsureAdditional(size_t add)
                {
                    if (oom)
                    {
                        return false;
                    }
                    if (len + add + 1 <= cap)
                    {
                        return true;
                    }
                    size_t newCap = cap * 2;
                    while (newCap < len + add + 1)
                    {
                        if (newCap > (size_t)INT32_MAX / 2)
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
                    return true;
                }

                bool Append(const char* s, size_t n)
                {
                    if (n == 0)
                    {
                        return !oom;
                    }
                    if (!EnsureAdditional(n))
                    {
                        return false;
                    }
                    js_memcpy_s(buf + len, cap - len, s, n);
                    len += n;
                    buf[len] = '\0';
                    return true;
                }

                bool AppendChar(char c)
                {
                    return Append(&c, 1);
                }

                bool AppendSz(const char* s)
                {
                    return s != nullptr ? Append(s, strlen(s)) : !oom;
                }

                bool AppendLower(const char* s, size_t n)
                {
                    if (!EnsureAdditional(n))
                    {
                        return false;
                    }
                    for (size_t i = 0; i < n; ++i)
                    {
                        buf[len + i] = ToAsciiLower(s[i]);
                    }
                    len += n;
                    buf[len] = '\0';
                    return true;
                }

                bool AppendUpper(const char* s, size_t n)
                {
                    if (!EnsureAdditional(n))
                    {
                        return false;
                    }
                    for (size_t i = 0; i < n; ++i)
                    {
                        buf[len + i] = ToAsciiUpper(s[i]);
                    }
                    len += n;
                    buf[len] = '\0';
                    return true;
                }

                bool AppendTitle(const char* s, size_t n)
                {
                    if (!EnsureAdditional(n))
                    {
                        return false;
                    }
                    for (size_t i = 0; i < n; ++i)
                    {
                        buf[len + i] = (i == 0) ? ToAsciiUpper(s[i]) : ToAsciiLower(s[i]);
                    }
                    len += n;
                    buf[len] = '\0';
                    return true;
                }

                Subtag AppendSliceLower(const char* s, size_t n)
                {
                    size_t start = len;
                    AppendLower(s, n);
                    return Subtag{ start, n };
                }

                Subtag AppendSliceSz(const char* s)
                {
                    size_t start = len;
                    size_t n = s != nullptr ? strlen(s) : 0;
                    Append(s, n);
                    return Subtag{ start, n };
                }

                Subtag AppendSliceUpper(const char* s, size_t n)
                {
                    size_t start = len;
                    AppendUpper(s, n);
                    return Subtag{ start, n };
                }

                Subtag AppendSliceTitle(const char* s, size_t n)
                {
                    size_t start = len;
                    AppendTitle(s, n);
                    return Subtag{ start, n };
                }

                void Clear()
                {
                    len = 0;
                    buf[0] = '\0';
                }

                const char* Data() const { return buf; }
                size_t Length() const { return len; }
                bool IsOom() const { return oom; }
            };

            bool IsAllAlpha(const char* s, size_t len)
            {
                for (size_t i = 0; i < len; ++i)
                {
                    if (!IsAsciiAlpha(s[i]))
                    {
                        return false;
                    }
                }
                return true;
            }

            bool IsAllDigit(const char* s, size_t len)
            {
                for (size_t i = 0; i < len; ++i)
                {
                    if (!IsAsciiDigit(s[i]))
                    {
                        return false;
                    }
                }
                return true;
            }

            bool IsAllAlnum(const char* s, size_t len)
            {
                for (size_t i = 0; i < len; ++i)
                {
                    if (!IsAsciiAlnum(s[i]))
                    {
                        return false;
                    }
                }
                return true;
            }

            bool SubtagEqualsIgnoreCase(const char* a, size_t aLen, const char* b, size_t bLen)
            {
                if (aLen != bLen)
                {
                    return false;
                }
                for (size_t i = 0; i < aLen; ++i)
                {
                    if (ToAsciiLower(a[i]) != ToAsciiLower(b[i]))
                    {
                        return false;
                    }
                }
                return true;
            }

            int CompareSlices(const char* a, size_t aLen, const char* b, size_t bLen)
            {
                size_t minLen = (aLen < bLen) ? aLen : bLen;
                for (size_t i = 0; i < minLen; ++i)
                {
                    unsigned char ac = static_cast<unsigned char>(a[i]);
                    unsigned char bc = static_cast<unsigned char>(b[i]);
                    if (ac != bc)
                    {
                        return (ac < bc) ? -1 : 1;
                    }
                }
                if (aLen == bLen)
                {
                    return 0;
                }
                return (aLen < bLen) ? -1 : 1;
            }

            bool SplitSubtags(const char* tag, size_t len, SmallList<Subtag, 64>& out)
            {
                if (tag == nullptr || len == 0 || tag[0] == '-' || tag[len - 1] == '-')
                {
                    return false;
                }
                size_t start = 0;
                for (size_t i = 0; i < len; ++i)
                {
                    char ch = tag[i];
                    if (ch == '-')
                    {
                        if (i == start || !out.Push(Subtag{ start, i - start }))
                        {
                            return false;
                        }
                        start = i + 1;
                    }
                    else if (!IsAsciiAlnum(ch))
                    {
                        return false;
                    }
                }
                if (start >= len)
                {
                    return false;
                }
                return out.Push(Subtag{ start, len - start });
            }

            struct Key2
            {
                char k0;
                char k1;
            };

            bool ParseUnicodeLanguageIdAt(const char* tag, const SmallList<Subtag, 64>& subtags, size_t& pos)
            {
                size_t count = subtags.Count();
                if (pos >= count)
                {
                    return false;
                }
                const Subtag& lang = subtags[pos];
                if (!IsUnicodeLanguageSubtag(tag + lang.start, lang.len))
                {
                    return false;
                }
                ++pos;

                if (pos < count && IsUnicodeScriptSubtag(tag + subtags[pos].start, subtags[pos].len))
                {
                    ++pos;
                }

                if (pos < count && IsUnicodeRegionSubtag(tag + subtags[pos].start, subtags[pos].len))
                {
                    ++pos;
                }

                size_t variantStart = pos;
                while (pos < count && IsUnicodeVariantSubtag(tag + subtags[pos].start, subtags[pos].len))
                {
                    const Subtag& cur = subtags[pos];
                    for (size_t prev = variantStart; prev < pos; ++prev)
                    {
                        if (SubtagEqualsIgnoreCase(
                                tag + subtags[prev].start, subtags[prev].len,
                                tag + cur.start, cur.len))
                        {
                            return false;
                        }
                    }
                    ++pos;
                }

                return true;
            }

            bool ValidateUnicodeLanguageTagInternal(const char* tag, size_t len, bool* outOom)
            {
                if (outOom != nullptr)
                {
                    *outOom = false;
                }

                SmallList<Subtag, 64> subtags;
                if (!SplitSubtags(tag, len, subtags))
                {
                    if (outOom != nullptr && subtags.IsOom())
                    {
                        *outOom = true;
                    }
                    return false;
                }

                size_t pos = 0;
                if (!ParseUnicodeLanguageIdAt(tag, subtags, pos))
                {
                    return false;
                }

                uint64 seenSingletons = 0;
                size_t count = subtags.Count();
                while (pos < count)
                {
                    const Subtag& sigTag = subtags[pos++];
                    if (sigTag.len != 1)
                    {
                        return false;
                    }

                    char sig = ToAsciiLower(tag[sigTag.start]);
                    uint64 bit = (sig >= '0' && sig <= '9')
                        ? (1ULL << (sig - '0'))
                        : (1ULL << (10 + (sig - 'a')));
                    if ((seenSingletons & bit) != 0)
                    {
                        return false;
                    }
                    seenSingletons |= bit;

                    if (sig == 'x')
                    {
                        if (pos >= count)
                        {
                            return false;
                        }
                        while (pos < count)
                        {
                            const Subtag& pu = subtags[pos++];
                            if (pu.len < 1 || pu.len > 8)
                            {
                                return false;
                            }
                        }
                        break;
                    }
                    else if (sig == 'u')
                    {
                        if (pos >= count)
                        {
                            return false;
                        }
                        bool hasUItem = false;
                        size_t attrStart = pos;
                        while (pos < count && subtags[pos].len >= 3 && subtags[pos].len <= 8)
                        {
                            const Subtag& attr = subtags[pos];
                            for (size_t prev = attrStart; prev < pos; ++prev)
                            {
                                if (SubtagEqualsIgnoreCase(
                                        tag + subtags[prev].start, subtags[prev].len,
                                        tag + attr.start, attr.len))
                                {
                                    return false;
                                }
                            }
                            hasUItem = true;
                            ++pos;
                        }

                        while (pos < count && subtags[pos].len != 1)
                        {
                            const Subtag& ukey = subtags[pos];
                            if (ukey.len != 2 ||
                                !IsAsciiAlnum(tag[ukey.start]) ||
                                !IsAsciiAlpha(tag[ukey.start + 1]))
                            {
                                return false;
                            }
                            hasUItem = true;
                            ++pos;

                            while (pos < count && subtags[pos].len >= 3 && subtags[pos].len <= 8)
                            {
                                ++pos;
                            }
                        }

                        if (!hasUItem)
                        {
                            return false;
                        }
                    }
                    else if (sig == 't')
                    {
                        if (pos >= count)
                        {
                            return false;
                        }
                        bool hasTContent = false;
                        if (IsUnicodeLanguageSubtag(tag + subtags[pos].start, subtags[pos].len))
                        {
                            if (!ParseUnicodeLanguageIdAt(tag, subtags, pos))
                            {
                                return false;
                            }
                            hasTContent = true;
                        }

                        SmallList<Key2, 16> seenTKeys;
                        while (pos < count && subtags[pos].len != 1)
                        {
                            const Subtag& tkey = subtags[pos];
                            if (tkey.len != 2 ||
                                !IsAsciiAlpha(tag[tkey.start]) ||
                                !IsAsciiDigit(tag[tkey.start + 1]))
                            {
                                return false;
                            }
                            char tk0 = ToAsciiLower(tag[tkey.start]);
                            char tk1 = tag[tkey.start + 1];
                            for (size_t i = 0; i < seenTKeys.Count(); ++i)
                            {
                                if (seenTKeys[i].k0 == tk0 && seenTKeys[i].k1 == tk1)
                                {
                                    return false;
                                }
                            }
                            if (!seenTKeys.Push(Key2{ tk0, tk1 }))
                            {
                                if (outOom != nullptr && seenTKeys.IsOom())
                                {
                                    *outOom = true;
                                }
                                return false;
                            }
                            ++pos;

                            size_t tvalCount = 0;
                            while (pos < count && subtags[pos].len >= 3 && subtags[pos].len <= 8)
                            {
                                ++tvalCount;
                                ++pos;
                            }
                            if (tvalCount == 0)
                            {
                                return false;
                            }
                            hasTContent = true;
                        }

                        if (!hasTContent)
                        {
                            return false;
                        }
                    }
                    else
                    {
                        size_t extCount = 0;
                        while (pos < count && subtags[pos].len >= 2 && subtags[pos].len <= 8)
                        {
                            ++extCount;
                            ++pos;
                        }
                        if (extCount == 0)
                        {
                            return false;
                        }
                    }
                }

                return true;
            }

            struct AliasBundles
            {
                UResourceBundle* meta;
                UResourceBundle* alias;
                UResourceBundle* language;
                UResourceBundle* script;
                UResourceBundle* territory;
                UResourceBundle* variant;
                UResourceBundle* subdivision;

                AliasBundles() :
                    meta(nullptr),
                    alias(nullptr),
                    language(nullptr),
                    script(nullptr),
                    territory(nullptr),
                    variant(nullptr),
                    subdivision(nullptr)
                {
                }

                ~AliasBundles()
                {
                    if (subdivision != nullptr) ures_close(subdivision);
                    if (variant != nullptr) ures_close(variant);
                    if (territory != nullptr) ures_close(territory);
                    if (script != nullptr) ures_close(script);
                    if (language != nullptr) ures_close(language);
                    if (alias != nullptr) ures_close(alias);
                    if (meta != nullptr) ures_close(meta);
                }

                void Init()
                {
                    UErrorCode err = U_ZERO_ERROR;
                    meta = ures_openDirect(nullptr, "metadata", &err);
                    if (U_FAILURE(err) || meta == nullptr)
                    {
                        if (meta != nullptr) { ures_close(meta); meta = nullptr; }
                        return;
                    }
                    alias = ures_getByKey(meta, "alias", nullptr, &err);
                    if (U_FAILURE(err) || alias == nullptr)
                    {
                        if (alias != nullptr) { ures_close(alias); alias = nullptr; }
                        return;
                    }

                    auto openSub = [&](const char* key) -> UResourceBundle* {
                        UErrorCode e = U_ZERO_ERROR;
                        UResourceBundle* sub = ures_getByKey(alias, key, nullptr, &e);
                        if (U_FAILURE(e) && sub != nullptr)
                        {
                            ures_close(sub);
                            sub = nullptr;
                        }
                        return U_SUCCESS(e) ? sub : nullptr;
                    };

                    language = openSub("language");
                    script = openSub("script");
                    territory = openSub("territory");
                    variant = openSub("variant");
                    subdivision = openSub("subdivision");
                }
            };

            bool LookupAliasReplacement(UResourceBundle* table, const char* key, CharBuffer& out)
            {
                out.Clear();
                if (table == nullptr || key == nullptr || key[0] == '\0')
                {
                    return false;
                }
                UErrorCode err = U_ZERO_ERROR;
                PlatformAgnostic::ICUHelpers::ScopedUResourceBundle entry(ures_getByKey(table, key, nullptr, &err));
                if (U_FAILURE(err) || entry == nullptr)
                {
                    return false;
                }
                int32_t repLen = 0;
                const UChar* repU = ures_getStringByKey(entry, "replacement", &repLen, &err);
                if (U_FAILURE(err) || repU == nullptr || repLen <= 0)
                {
                    return false;
                }
                for (int32_t i = 0; i < repLen; ++i)
                {
                    if (!out.AppendChar(static_cast<char>(repU[i] & 0xFF)))
                    {
                        return false;
                    }
                }
                return true;
            }

            void ExtractFirstSpaceToken(const char* src, size_t len, char* dst, size_t dstCap)
            {
                size_t n = 0;
                while (n < len && src[n] != ' ' && n + 1 < dstCap)
                {
                    dst[n] = src[n];
                    ++n;
                }
                dst[n] = '\0';
            }

            bool SpaceListContainsToken(const char* list, size_t listLen, const char* token, size_t tokenLen)
            {
                if (tokenLen == 0)
                {
                    return false;
                }
                size_t pos = 0;
                while (pos < listLen)
                {
                    while (pos < listLen && list[pos] == ' ')
                    {
                        ++pos;
                    }
                    size_t start = pos;
                    while (pos < listLen && list[pos] != ' ')
                    {
                        ++pos;
                    }
                    if (pos - start == tokenLen && strncmp(list + start, token, tokenLen) == 0)
                    {
                        return true;
                    }
                }
                return false;
            }

            struct CanonicalLangId
            {
                Subtag lang;
                Subtag script;
                Subtag region;
                SmallList<Subtag, 16> variants;

                CanonicalLangId() : lang{ 0, 0 }, script{ 0, 0 }, region{ 0, 0 } {}
            };

            bool BuildLanguageIdFromSubtags(
                const char* src,
                const SmallList<Subtag, 64>& subtags,
                size_t& pos,
                CharBuffer& pool,
                CanonicalLangId& out)
            {
                size_t count = subtags.Count();
                if (pos >= count || !IsUnicodeLanguageSubtag(src + subtags[pos].start, subtags[pos].len))
                {
                    return false;
                }
                out.lang = pool.AppendSliceLower(src + subtags[pos].start, subtags[pos].len);
                ++pos;

                if (pos < count && IsUnicodeScriptSubtag(src + subtags[pos].start, subtags[pos].len))
                {
                    out.script = pool.AppendSliceTitle(src + subtags[pos].start, subtags[pos].len);
                    ++pos;
                }
                if (pos < count && IsUnicodeRegionSubtag(src + subtags[pos].start, subtags[pos].len))
                {
                    out.region = pool.AppendSliceUpper(src + subtags[pos].start, subtags[pos].len);
                    ++pos;
                }
                while (pos < count && IsUnicodeVariantSubtag(src + subtags[pos].start, subtags[pos].len))
                {
                    Subtag v = pool.AppendSliceLower(src + subtags[pos].start, subtags[pos].len);
                    out.variants.Push(v);
                    ++pos;
                }
                return !pool.IsOom() && !out.variants.IsOom();
            }

            void ApplyLanguageReplacementTokens(
                CanonicalLangId& lid,
                const char* rep,
                size_t repLen,
                CharBuffer& pool)
            {
                char firstRep[64];
                ExtractFirstSpaceToken(rep, repLen, firstRep, sizeof(firstRep));

                size_t tokStart = 0;
                size_t firstLen = strlen(firstRep);
                size_t idx = 0;
                while (tokStart <= firstLen)
                {
                    size_t tokEnd = tokStart;
                    while (tokEnd < firstLen && firstRep[tokEnd] != '_' && firstRep[tokEnd] != '-')
                    {
                        ++tokEnd;
                    }
                    size_t tokLen = tokEnd - tokStart;
                    const char* tok = firstRep + tokStart;
                    if (tokLen > 0)
                    {
                        if (idx == 0)
                        {
                            lid.lang = pool.AppendSliceLower(tok, tokLen);
                        }
                        else if (IsUnicodeScriptSubtag(tok, tokLen))
                        {
                            if (lid.script.len == 0)
                            {
                                lid.script = pool.AppendSliceTitle(tok, tokLen);
                            }
                        }
                        else if (IsUnicodeRegionSubtag(tok, tokLen))
                        {
                            if (lid.region.len == 0)
                            {
                                lid.region = pool.AppendSliceUpper(tok, tokLen);
                            }
                        }
                        else if (IsUnicodeVariantSubtag(tok, tokLen))
                        {
                            lid.variants.Push(pool.AppendSliceLower(tok, tokLen));
                        }
                    }
                    ++idx;
                    if (tokEnd >= firstLen)
                    {
                        break;
                    }
                    tokStart = tokEnd + 1;
                }
            }

            void SortAndDedupVariants(CanonicalLangId& lid, const CharBuffer& pool)
            {
                size_t n = lid.variants.Count();
                if (n <= 1)
                {
                    return;
                }
                const char* base = pool.Data();
                for (size_t i = 0; i + 1 < n; ++i)
                {
                    for (size_t j = i + 1; j < n; ++j)
                    {
                        if (CompareSlices(
                                base + lid.variants[j].start, lid.variants[j].len,
                                base + lid.variants[i].start, lid.variants[i].len) < 0)
                        {
                            Subtag tmp = lid.variants[i];
                            lid.variants[i] = lid.variants[j];
                            lid.variants[j] = tmp;
                        }
                    }
                }
                size_t writeIdx = 0;
                for (size_t i = 0; i < n; ++i)
                {
                    if (writeIdx == 0 ||
                        CompareSlices(
                            base + lid.variants[writeIdx - 1].start, lid.variants[writeIdx - 1].len,
                            base + lid.variants[i].start, lid.variants[i].len) != 0)
                    {
                        lid.variants[writeIdx++] = lid.variants[i];
                    }
                }
                while (lid.variants.Count() > writeIdx)
                {
                    lid.variants.RemoveAt(lid.variants.Count() - 1);
                }
            }

            bool CanonicalizeLangId(
                CanonicalLangId& lid,
                CharBuffer& pool,
                const AliasBundles& bundles)
            {
                SortAndDedupVariants(lid, pool);

                CharBuffer repBuf;
                CharBuffer keyBuf;
                bool langMatched = false;

                // Step 1: lang[_region][_variant]
                if (lid.region.len > 0 && lid.variants.Count() > 0)
                {
                    for (size_t i = 0; i < lid.variants.Count(); ++i)
                    {
                        keyBuf.Clear();
                        keyBuf.Append(pool.Data() + lid.lang.start, lid.lang.len);
                        keyBuf.AppendChar('_');
                        keyBuf.Append(pool.Data() + lid.region.start, lid.region.len);
                        keyBuf.AppendChar('_');
                        keyBuf.Append(pool.Data() + lid.variants[i].start, lid.variants[i].len);
                        if (LookupAliasReplacement(bundles.language, keyBuf.Data(), repBuf))
                        {
                            lid.variants.RemoveAt(i);
                            lid.region = Subtag{ 0, 0 };
                            ApplyLanguageReplacementTokens(lid, repBuf.Data(), repBuf.Length(), pool);
                            langMatched = true;
                            break;
                        }
                    }
                }

                // Step 2: lang[_variant]
                if (!langMatched && lid.variants.Count() > 0)
                {
                    for (size_t i = 0; i < lid.variants.Count(); ++i)
                    {
                        keyBuf.Clear();
                        keyBuf.Append(pool.Data() + lid.lang.start, lid.lang.len);
                        keyBuf.AppendChar('_');
                        keyBuf.Append(pool.Data() + lid.variants[i].start, lid.variants[i].len);
                        if (LookupAliasReplacement(bundles.language, keyBuf.Data(), repBuf))
                        {
                            lid.variants.RemoveAt(i);
                            ApplyLanguageReplacementTokens(lid, repBuf.Data(), repBuf.Length(), pool);
                            langMatched = true;
                            break;
                        }
                    }
                    // Legacy ICU rule for hy-arevela -> hy when script & region are absent
                    if (!langMatched &&
                        lid.lang.len == 2 &&
                        strncmp(pool.Data() + lid.lang.start, "hy", 2) == 0 &&
                        lid.script.len == 0 &&
                        lid.region.len == 0)
                    {
                        for (size_t i = 0; i < lid.variants.Count(); ++i)
                        {
                            if (lid.variants[i].len == 7 &&
                                strncmp(pool.Data() + lid.variants[i].start, "arevela", 7) == 0)
                            {
                                lid.variants.RemoveAt(i);
                                langMatched = true;
                                break;
                            }
                        }
                    }
                }

                // Step 3: lang[_region]
                if (!langMatched && lid.region.len > 0)
                {
                    keyBuf.Clear();
                    keyBuf.Append(pool.Data() + lid.lang.start, lid.lang.len);
                    keyBuf.AppendChar('_');
                    keyBuf.Append(pool.Data() + lid.region.start, lid.region.len);
                    if (LookupAliasReplacement(bundles.language, keyBuf.Data(), repBuf))
                    {
                        lid.region = Subtag{ 0, 0 };
                        ApplyLanguageReplacementTokens(lid, repBuf.Data(), repBuf.Length(), pool);
                        langMatched = true;
                    }
                }

                // Step 4: lang
                if (!langMatched)
                {
                    keyBuf.Clear();
                    keyBuf.Append(pool.Data() + lid.lang.start, lid.lang.len);
                    if (LookupAliasReplacement(bundles.language, keyBuf.Data(), repBuf))
                    {
                        ApplyLanguageReplacementTokens(lid, repBuf.Data(), repBuf.Length(), pool);
                    }
                }

                // Script alias
                if (lid.script.len > 0)
                {
                    keyBuf.Clear();
                    keyBuf.Append(pool.Data() + lid.script.start, lid.script.len);
                    if (LookupAliasReplacement(bundles.script, keyBuf.Data(), repBuf))
                    {
                        char tok[16];
                        ExtractFirstSpaceToken(repBuf.Data(), repBuf.Length(), tok, sizeof(tok));
                        if (tok[0] != '\0')
                        {
                            lid.script = pool.AppendSliceTitle(tok, strlen(tok));
                        }
                    }
                }

                // Territory alias
                if (lid.region.len > 0)
                {
                    keyBuf.Clear();
                    keyBuf.Append(pool.Data() + lid.region.start, lid.region.len);
                    if (LookupAliasReplacement(bundles.territory, keyBuf.Data(), repBuf))
                    {
                        char chosen[16];
                        ExtractFirstSpaceToken(repBuf.Data(), repBuf.Length(), chosen, sizeof(chosen));

                        bool hasMultiple = false;
                        for (size_t i = 0; i < repBuf.Length(); ++i)
                        {
                            if (repBuf.Data()[i] == ' ')
                            {
                                hasMultiple = true;
                                break;
                            }
                        }

                        if (hasMultiple)
                        {
                            char baseIcu[32];
                            CharBuffer baseBuf;
                            baseBuf.Append(pool.Data() + lid.lang.start, lid.lang.len);
                            if (lid.script.len > 0)
                            {
                                baseBuf.AppendChar('_');
                                baseBuf.Append(pool.Data() + lid.script.start, lid.script.len);
                            }
                            if (baseBuf.Length() < sizeof(baseIcu))
                            {
                                js_memcpy_s(baseIcu, sizeof(baseIcu), baseBuf.Data(), baseBuf.Length() + 1);
                                UErrorCode err = U_ZERO_ERROR;
                                char maxLoc[ULOC_FULLNAME_CAPACITY] = { 0 };
                                int32_t mlen = uloc_addLikelySubtags(baseIcu, maxLoc, static_cast<int32_t>(sizeof(maxLoc)), &err);
                                if (U_SUCCESS(err) && mlen > 0)
                                {
                                    char country[16] = { 0 };
                                    UErrorCode cErr = U_ZERO_ERROR;
                                    int32_t clen = uloc_getCountry(maxLoc, country, static_cast<int32_t>(sizeof(country)), &cErr);
                                    if (U_SUCCESS(cErr) && clen > 0 &&
                                        SpaceListContainsToken(repBuf.Data(), repBuf.Length(), country, static_cast<size_t>(clen)))
                                    {
                                        js_memcpy_s(chosen, sizeof(chosen), country, static_cast<size_t>(clen) + 1);
                                    }
                                }
                            }
                        }

                        if (chosen[0] != '\0')
                        {
                            lid.region = pool.AppendSliceUpper(chosen, strlen(chosen));
                        }
                    }
                }

                // Paired variant alias: hepburn-heploc -> alalc97
                bool hasHepburn = false;
                size_t hepburnIdx = static_cast<size_t>(-1);
                bool hasHeploc = false;
                for (size_t i = 0; i < lid.variants.Count(); ++i)
                {
                    const char* vptr = pool.Data() + lid.variants[i].start;
                    size_t vlen = lid.variants[i].len;
                    if (vlen == 7 && strncmp(vptr, "hepburn", 7) == 0)
                    {
                        hasHepburn = true;
                        hepburnIdx = i;
                    }
                    else if (vlen == 6 && strncmp(vptr, "heploc", 6) == 0)
                    {
                        hasHeploc = true;
                    }
                }
                if (hasHepburn && hasHeploc)
                {
                    lid.variants.RemoveAt(hepburnIdx);
                }

                // Variant alias
                for (size_t i = 0; i < lid.variants.Count(); ++i)
                {
                    keyBuf.Clear();
                    keyBuf.Append(pool.Data() + lid.variants[i].start, lid.variants[i].len);
                    if (LookupAliasReplacement(bundles.variant, keyBuf.Data(), repBuf))
                    {
                        char tok[32];
                        ExtractFirstSpaceToken(repBuf.Data(), repBuf.Length(), tok, sizeof(tok));
                        if (tok[0] != '\0')
                        {
                            lid.variants[i] = pool.AppendSliceLower(tok, strlen(tok));
                        }
                    }
                }

                SortAndDedupVariants(lid, pool);
                return !pool.IsOom() && !repBuf.IsOom() && !keyBuf.IsOom();
            }

            bool EmitLangId(const CanonicalLangId& lid, const CharBuffer& pool, CharBuffer& out, bool lowercaseAll = false)
            {
                const char* base = pool.Data();
                if (!out.Append(base + lid.lang.start, lid.lang.len))
                {
                    return false;
                }
                if (lid.script.len > 0)
                {
                    if (!out.AppendChar('-') ||
                        !(lowercaseAll ? out.AppendLower(base + lid.script.start, lid.script.len)
                                       : out.Append(base + lid.script.start, lid.script.len)))
                    {
                        return false;
                    }
                }
                if (lid.region.len > 0)
                {
                    if (!out.AppendChar('-') ||
                        !(lowercaseAll ? out.AppendLower(base + lid.region.start, lid.region.len)
                                       : out.Append(base + lid.region.start, lid.region.len)))
                    {
                        return false;
                    }
                }
                for (size_t i = 0; i < lid.variants.Count(); ++i)
                {
                    if (!out.AppendChar('-') || !out.Append(base + lid.variants[i].start, lid.variants[i].len))
                    {
                        return false;
                    }
                }
                return true;
            }

            struct KeyValueSlice
            {
                Subtag key;
                Subtag value;
            };

            struct ExtSpan
            {
                char singleton;
                size_t startSubtag;
                size_t endSubtag;
            };

            bool CanonicalizeFullTag(
                const char* tag,
                size_t len,
                CharBuffer& out,
                bool* outOom)
            {
                if (outOom != nullptr)
                {
                    *outOom = false;
                }

                SmallList<Subtag, 64> subtags;
                if (!SplitSubtags(tag, len, subtags))
                {
                    if (outOom != nullptr && subtags.IsOom())
                    {
                        *outOom = true;
                    }
                    return false;
                }

                CharBuffer pool;
                CanonicalLangId mainLid;
                size_t pos = 0;
                if (!BuildLanguageIdFromSubtags(tag, subtags, pos, pool, mainLid))
                {
                    if (outOom != nullptr && (pool.IsOom() || mainLid.variants.IsOom()))
                    {
                        *outOom = true;
                    }
                    return false;
                }

                AliasBundles bundles;
                bundles.Init();

                if (!CanonicalizeLangId(mainLid, pool, bundles))
                {
                    if (outOom != nullptr)
                    {
                        *outOom = true;
                    }
                    return false;
                }

                if (!EmitLangId(mainLid, pool, out))
                {
                    if (outOom != nullptr)
                    {
                        *outOom = true;
                    }
                    return false;
                }

                SmallList<ExtSpan, 16> exts;
                ExtSpan privateUse = { '\0', 0, 0 };
                bool hasPrivateUse = false;

                size_t count = subtags.Count();
                while (pos < count)
                {
                    char sig = ToAsciiLower(tag[subtags[pos].start]);
                    ++pos;
                    size_t startIdx = pos;
                    if (sig == 'x')
                    {
                        privateUse = ExtSpan{ 'x', startIdx, count };
                        hasPrivateUse = true;
                        break;
                    }
                    while (pos < count && subtags[pos].len != 1)
                    {
                        ++pos;
                    }
                    if (!exts.Push(ExtSpan{ sig, startIdx, pos }))
                    {
                        if (outOom != nullptr)
                        {
                            *outOom = true;
                        }
                        return false;
                    }
                }

                // Sort extensions alphabetically by singleton
                for (size_t i = 0; i + 1 < exts.Count(); ++i)
                {
                    for (size_t j = i + 1; j < exts.Count(); ++j)
                    {
                        if (static_cast<unsigned char>(exts[j].singleton) < static_cast<unsigned char>(exts[i].singleton))
                        {
                            ExtSpan tmp = exts[i];
                            exts[i] = exts[j];
                            exts[j] = tmp;
                        }
                    }
                }

                for (size_t e = 0; e < exts.Count(); ++e)
                {
                    const ExtSpan& ext = exts[e];
                    if (!out.AppendChar('-') || !out.AppendChar(ext.singleton))
                    {
                        if (outOom != nullptr) *outOom = true;
                        return false;
                    }

                    if (ext.singleton == 'u')
                    {
                        SmallList<Subtag, 16> attrs;
                        SmallList<KeyValueSlice, 16> kws;
                        size_t p = ext.startSubtag;

                        while (p < ext.endSubtag && subtags[p].len >= 3)
                        {
                            attrs.Push(pool.AppendSliceLower(tag + subtags[p].start, subtags[p].len));
                            ++p;
                        }

                        // Sort attributes
                        for (size_t i = 0; i + 1 < attrs.Count(); ++i)
                        {
                            for (size_t j = i + 1; j < attrs.Count(); ++j)
                            {
                                if (CompareSlices(
                                        pool.Data() + attrs[j].start, attrs[j].len,
                                        pool.Data() + attrs[i].start, attrs[i].len) < 0)
                                {
                                    Subtag tmp = attrs[i];
                                    attrs[i] = attrs[j];
                                    attrs[j] = tmp;
                                }
                            }
                        }

                        while (p < ext.endSubtag)
                        {
                            Subtag keySub = pool.AppendSliceLower(tag + subtags[p].start, subtags[p].len);
                            ++p;

                            CharBuffer typeJoin;
                            size_t valCount = 0;
                            while (p < ext.endSubtag && subtags[p].len >= 3)
                            {
                                if (valCount > 0)
                                {
                                    typeJoin.AppendChar('-');
                                }
                                typeJoin.AppendLower(tag + subtags[p].start, subtags[p].len);
                                ++valCount;
                                ++p;
                            }

                            // Duplicate keyword check: first wins per UTS #35
                            bool dupKey = false;
                            for (size_t i = 0; i < kws.Count(); ++i)
                            {
                                if (CompareSlices(
                                        pool.Data() + kws[i].key.start, kws[i].key.len,
                                        pool.Data() + keySub.start, keySub.len) == 0)
                                {
                                    dupKey = true;
                                    break;
                                }
                            }
                            if (dupKey)
                            {
                                continue;
                            }

                            Subtag valSub = { 0, 0 };
                            if (valCount > 0)
                            {
                                char keySz[4] = { 0 };
                                js_memcpy_s(keySz, sizeof(keySz), pool.Data() + keySub.start, keySub.len);
                                keySz[keySub.len] = '\0';

                                if ((strcmp(keySz, "sd") == 0 || strcmp(keySz, "rg") == 0) && valCount == 1)
                                {
                                    CharBuffer subRep;
                                    if (LookupAliasReplacement(bundles.subdivision, typeJoin.Data(), subRep))
                                    {
                                        char tok[32];
                                        ExtractFirstSpaceToken(subRep.Data(), subRep.Length(), tok, sizeof(tok));
                                        if (tok[0] != '\0')
                                        {
                                            typeJoin.Clear();
                                            typeJoin.AppendLower(tok, strlen(tok));
                                        }
                                    }
                                }
                                else
                                {
                                    const char* canonType = uloc_toUnicodeLocaleType(keySz, typeJoin.Data());
                                    if (canonType != nullptr && canonType[0] != '\0' && canonType != typeJoin.Data())
                                    {
                                        CharBuffer tmpCanon;
                                        tmpCanon.AppendLower(canonType, strlen(canonType));
                                        typeJoin.Clear();
                                        typeJoin.Append(tmpCanon.Data(), tmpCanon.Length());
                                    }
                                }

                                if (strcmp(typeJoin.Data(), "true") != 0)
                                {
                                    valSub = pool.AppendSliceLower(typeJoin.Data(), typeJoin.Length());
                                }
                            }

                            kws.Push(KeyValueSlice{ keySub, valSub });
                        }

                        // Sort keywords by key
                        for (size_t i = 0; i + 1 < kws.Count(); ++i)
                        {
                            for (size_t j = i + 1; j < kws.Count(); ++j)
                            {
                                if (CompareSlices(
                                        pool.Data() + kws[j].key.start, kws[j].key.len,
                                        pool.Data() + kws[i].key.start, kws[i].key.len) < 0)
                                {
                                    KeyValueSlice tmp = kws[i];
                                    kws[i] = kws[j];
                                    kws[j] = tmp;
                                }
                            }
                        }

                        for (size_t i = 0; i < attrs.Count(); ++i)
                        {
                            out.AppendChar('-');
                            out.Append(pool.Data() + attrs[i].start, attrs[i].len);
                        }
                        for (size_t i = 0; i < kws.Count(); ++i)
                        {
                            out.AppendChar('-');
                            out.Append(pool.Data() + kws[i].key.start, kws[i].key.len);
                            if (kws[i].value.len > 0)
                            {
                                out.AppendChar('-');
                                out.Append(pool.Data() + kws[i].value.start, kws[i].value.len);
                            }
                        }
                    }
                    else if (ext.singleton == 't')
                    {
                        size_t p = ext.startSubtag;
                        if (p < ext.endSubtag && IsUnicodeLanguageSubtag(tag + subtags[p].start, subtags[p].len))
                        {
                            CanonicalLangId tlang;
                            if (!BuildLanguageIdFromSubtags(tag, subtags, p, pool, tlang) ||
                                !CanonicalizeLangId(tlang, pool, bundles))
                            {
                                if (outOom != nullptr) *outOom = true;
                                return false;
                            }
                            out.AppendChar('-');
                            EmitLangId(tlang, pool, out, true);
                        }

                        SmallList<KeyValueSlice, 16> tfields;
                        while (p < ext.endSubtag)
                        {
                            Subtag tkSub = pool.AppendSliceLower(tag + subtags[p].start, subtags[p].len);
                            ++p;

                            CharBuffer tvalJoin;
                            size_t valCount = 0;
                            while (p < ext.endSubtag && subtags[p].len >= 3)
                            {
                                if (valCount > 0)
                                {
                                    tvalJoin.AppendChar('-');
                                }
                                tvalJoin.AppendLower(tag + subtags[p].start, subtags[p].len);
                                ++valCount;
                                ++p;
                            }

                            char tkeySz[4] = { 0 };
                            js_memcpy_s(tkeySz, sizeof(tkeySz), pool.Data() + tkSub.start, tkSub.len);
                            tkeySz[tkSub.len] = '\0';

                            const char* canonTVal = uloc_toUnicodeLocaleType(tkeySz, tvalJoin.Data());
                            Subtag tvSub = (canonTVal != nullptr && canonTVal[0] != '\0')
                                ? pool.AppendSliceLower(canonTVal, strlen(canonTVal))
                                : pool.AppendSliceLower(tvalJoin.Data(), tvalJoin.Length());
                            tfields.Push(KeyValueSlice{ tkSub, tvSub });
                        }

                        for (size_t i = 0; i + 1 < tfields.Count(); ++i)
                        {
                            for (size_t j = i + 1; j < tfields.Count(); ++j)
                            {
                                if (CompareSlices(
                                        pool.Data() + tfields[j].key.start, tfields[j].key.len,
                                        pool.Data() + tfields[i].key.start, tfields[i].key.len) < 0)
                                {
                                    KeyValueSlice tmp = tfields[i];
                                    tfields[i] = tfields[j];
                                    tfields[j] = tmp;
                                }
                            }
                        }

                        for (size_t i = 0; i < tfields.Count(); ++i)
                        {
                            out.AppendChar('-');
                            out.Append(pool.Data() + tfields[i].key.start, tfields[i].key.len);
                            out.AppendChar('-');
                            out.Append(pool.Data() + tfields[i].value.start, tfields[i].value.len);
                        }
                    }
                    else
                    {
                        for (size_t p = ext.startSubtag; p < ext.endSubtag; ++p)
                        {
                            out.AppendChar('-');
                            out.AppendLower(tag + subtags[p].start, subtags[p].len);
                        }
                    }
                }

                if (hasPrivateUse)
                {
                    out.Append("-x", 2);
                    for (size_t p = privateUse.startSubtag; p < privateUse.endSubtag; ++p)
                    {
                        out.AppendChar('-');
                        out.AppendLower(tag + subtags[p].start, subtags[p].len);
                    }
                }

                if (out.IsOom() || pool.IsOom())
                {
                    if (outOom != nullptr)
                    {
                        *outOom = true;
                    }
                    return false;
                }
                return true;
            }

            JavascriptString* TransformLikelySubtags(
                const char* canonicalTag,
                size_t len,
                bool maximize,
                ScriptContext* scriptContext)
            {
                SmallList<Subtag, 64> subtags;
                if (!SplitSubtags(canonicalTag, len, subtags))
                {
                    return CreateAsciiString(canonicalTag, len, scriptContext);
                }

                CharBuffer pool;
                CanonicalLangId lid;
                size_t pos = 0;
                if (!BuildLanguageIdFromSubtags(canonicalTag, subtags, pos, pool, lid))
                {
                    return CreateAsciiString(canonicalTag, len, scriptContext);
                }

                CharBuffer icuBase;
                icuBase.Append(pool.Data() + lid.lang.start, lid.lang.len);
                if (lid.script.len > 0)
                {
                    icuBase.AppendChar('_');
                    icuBase.Append(pool.Data() + lid.script.start, lid.script.len);
                }
                if (lid.region.len > 0)
                {
                    icuBase.AppendChar('_');
                    icuBase.Append(pool.Data() + lid.region.start, lid.region.len);
                }

                if (icuBase.IsOom())
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }

                UErrorCode err = U_ZERO_ERROR;
                char transformedIcu[ULOC_FULLNAME_CAPACITY] = { 0 };
                int32_t tlen = maximize
                    ? uloc_addLikelySubtags(icuBase.Data(), transformedIcu, static_cast<int32_t>(sizeof(transformedIcu)), &err)
                    : uloc_minimizeSubtags(icuBase.Data(), transformedIcu, static_cast<int32_t>(sizeof(transformedIcu)), &err);
                if (U_FAILURE(err) || tlen <= 0 || static_cast<size_t>(tlen) >= sizeof(transformedIcu))
                {
                    return CreateAsciiString(canonicalTag, len, scriptContext);
                }

                char bcpBase[ULOC_FULLNAME_CAPACITY] = { 0 };
                int32_t blen = uloc_toLanguageTag(transformedIcu, bcpBase, static_cast<int32_t>(sizeof(bcpBase)), FALSE, &err);
                if (U_FAILURE(err) || blen <= 0 || static_cast<size_t>(blen) >= sizeof(bcpBase))
                {
                    return CreateAsciiString(canonicalTag, len, scriptContext);
                }

                CharBuffer finalOut;
                finalOut.Append(bcpBase, static_cast<size_t>(blen));
                for (size_t i = 0; i < lid.variants.Count(); ++i)
                {
                    finalOut.AppendChar('-');
                    finalOut.Append(pool.Data() + lid.variants[i].start, lid.variants[i].len);
                }
                if (pos < subtags.Count())
                {
                    size_t extStart = subtags[pos].start;
                    finalOut.AppendChar('-');
                    finalOut.Append(canonicalTag + extStart, len - extStart);
                }
                if (finalOut.IsOom())
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                return CreateAsciiString(finalOut.Data(), finalOut.Length(), scriptContext);
            }

            bool IcuLocaleIdMatchesTag(const char* icuLocaleId, const char* bcp47NoExt, size_t tagLen)
            {
                if (icuLocaleId == nullptr)
                {
                    return false;
                }
                size_t icuLen = strlen(icuLocaleId);
                if (icuLen != tagLen)
                {
                    return false;
                }
                for (size_t i = 0; i < tagLen; ++i)
                {
                    char a = icuLocaleId[i];
                    char b = bcp47NoExt[i];
                    if (a == '_')
                    {
                        a = '-';
                    }
                    if (ToAsciiLower(a) != ToAsciiLower(b))
                    {
                        return false;
                    }
                }
                return true;
            }

            bool IsExactLocaleAvailable(IntlModernKind kind, const char* bcp47NoExt, size_t tagLen)
            {
                if (kind == IntlModernKind::Segmenter)
                {
                    int32_t total = ubrk_countAvailable();
                    for (int32_t i = 0; i < total; ++i)
                    {
                        if (IcuLocaleIdMatchesTag(ubrk_getAvailable(i), bcp47NoExt, tagLen))
                        {
                            return true;
                        }
                    }
                    return false;
                }

                int32_t total = uloc_countAvailable();
                for (int32_t i = 0; i < total; ++i)
                {
                    if (IcuLocaleIdMatchesTag(uloc_getAvailable(i), bcp47NoExt, tagLen))
                    {
                        return true;
                    }
                }
                return false;
            }

            bool StripUnicodeAndPrivateExtensions(const char* tag, size_t len, size_t* outBaseLen)
            {
                SmallList<Subtag, 64> subtags;
                if (!SplitSubtags(tag, len, subtags))
                {
                    return false;
                }
                size_t endLen = len;
                for (size_t i = 1; i < subtags.Count(); ++i)
                {
                    if (subtags[i].len == 1)
                    {
                        endLen = subtags[i].start - 1;
                        break;
                    }
                }
                *outBaseLen = endLen;
                return true;
            }

            bool BestAvailableLocale(
                IntlModernKind kind,
                const char* candidate,
                size_t candidateLen,
                char* outBuf,
                size_t outCap,
                size_t* outLen)
            {
                if (candidateLen == 0 || candidateLen >= outCap)
                {
                    return false;
                }
                js_memcpy_s(outBuf, outCap, candidate, candidateLen);
                outBuf[candidateLen] = '\0';
                size_t curLen = candidateLen;

                while (curLen > 0)
                {
                    if (IsExactLocaleAvailable(kind, outBuf, curLen))
                    {
                        *outLen = curLen;
                        return true;
                    }

                    size_t pos = curLen;
                    while (pos > 0 && outBuf[pos - 1] != '-')
                    {
                        --pos;
                    }
                    if (pos == 0)
                    {
                        return false;
                    }
                    --pos;
                    if (pos >= 2 && outBuf[pos - 2] == '-')
                    {
                        pos -= 2;
                    }
                    curLen = pos;
                    outBuf[curLen] = '\0';
                }

                return false;
            }

            bool ExtractNumberingSystemFromTag(const char* tag, size_t len, char* outNu, size_t outCap)
            {
                SmallList<Subtag, 64> subtags;
                if (!SplitSubtags(tag, len, subtags))
                {
                    return false;
                }
                size_t count = subtags.Count();
                for (size_t i = 1; i < count; ++i)
                {
                    if (subtags[i].len == 1)
                    {
                        char sig = ToAsciiLower(tag[subtags[i].start]);
                        if (sig == 'x')
                        {
                            break;
                        }
                        if (sig == 'u')
                        {
                            size_t pos = i + 1;
                            while (pos < count && subtags[pos].len >= 3)
                            {
                                ++pos;
                            }
                            while (pos < count && subtags[pos].len != 1)
                            {
                                const Subtag& k = subtags[pos++];
                                char k0 = ToAsciiLower(tag[k.start]);
                                char k1 = (k.len == 2) ? ToAsciiLower(tag[k.start + 1]) : '\0';
                                size_t valStart = pos;
                                size_t valCount = 0;
                                while (pos < count && subtags[pos].len >= 3)
                                {
                                    ++valCount;
                                    ++pos;
                                }
                                if (k.len == 2 && k0 == 'n' && k1 == 'u' && valCount == 1)
                                {
                                    const Subtag& val = subtags[valStart];
                                    if (val.len < outCap)
                                    {
                                        for (size_t j = 0; j < val.len; ++j)
                                        {
                                            outNu[j] = ToAsciiLower(tag[val.start + j]);
                                        }
                                        outNu[val.len] = '\0';
                                        return true;
                                    }
                                    return false;
                                }
                            }
                            break;
                        }
                    }
                }
                return false;
            }

            void GetDefaultNumberingSystemForLocale(const char* icuLocaleId, char* outNu, size_t outCap)
            {
                UErrorCode err = U_ZERO_ERROR;
                PlatformAgnostic::ICUHelpers::ScopedUNumberingSystem ns(unumsys_open(icuLocaleId, &err));
                if (U_SUCCESS(err) && ns != nullptr && !unumsys_isAlgorithmic(ns))
                {
                    const char* name = unumsys_getName(ns);
                    if (name != nullptr)
                    {
                        size_t nlen = strlen(name);
                        if (nlen > 0 && nlen < outCap)
                        {
                            js_memcpy_s(outNu, outCap, name, nlen + 1);
                            return;
                        }
                    }
                }
                js_memcpy_s(outNu, outCap, "latn", 5);
            }
        } // namespace

        IntlModernState::IntlModernState(IntlModernKind kind) :
            magic(&gIntlModernStateSentinel),
            kind(kind)
        {
        }

        bool IntlModernState::IsValidState() const
        {
            return magic == &gIntlModernStateSentinel;
        }

        void IntlModernState::InvalidateMagic()
        {
            magic = nullptr;
        }

        bool SetState(DynamicObject* obj, IntlModernState* state)
        {
            Assert(obj != nullptr && state != nullptr);
            return obj->SetInternalProperty(InternalPropertyIds::HiddenObject, state, PropertyOperation_None, nullptr) != FALSE;
        }

        IntlModernState* TryGetRawState(Var val, IntlModernKind kind)
        {
            DynamicObject* obj = DynamicObject::TryVarToBaseDynamicObject(val);
            if (obj == nullptr)
            {
                return nullptr;
            }

            Var hidden = nullptr;
            if (!obj->GetInternalProperty(obj, InternalPropertyIds::HiddenObject, &hidden, nullptr, obj->GetScriptContext()) ||
                hidden == nullptr ||
                TaggedInt::Is(hidden) ||
                JavascriptNumber::Is_NoTaggedIntCheck(hidden))
            {
                return nullptr;
            }

            if ((reinterpret_cast<uintptr_t>(hidden) & (sizeof(void*) - 1)) != 0)
            {
                return nullptr;
            }

            IntlModernState* state = reinterpret_cast<IntlModernState*>(hidden);
            if (!state->IsValidState() || state->GetKind() != kind)
            {
                return nullptr;
            }

            return state;
        }

        void ThrowInvalidReceiverTypeError(ScriptContext* scriptContext, const char16* methodName)
        {
            JavascriptError::ThrowTypeError(
                scriptContext,
                JSERR_FunctionArgument_NeedObject,
                methodName != nullptr ? methodName : _u("Intl"));
        }

        JavascriptString* GetLocaleTagFromState(IntlModernState* localeState)
        {
            if (localeState == nullptr || !localeState->IsValidState())
            {
                return nullptr;
            }
            return localeState->GetLocaleTag();
        }

        JavascriptString* TryGetLocaleTagIfLocaleObject(Var val)
        {
            IntlModernState* state = TryGetRawState(val, IntlModernKind::Locale);
            return GetLocaleTagFromState(state);
        }

        DynamicObject* OrdinaryCreateFromConstructor(
            Var newTarget,
            ScriptContext* scriptContext,
            DynamicObject* (JavascriptLibrary::*getProto)() const)
        {
            Assert(newTarget != nullptr && scriptContext != nullptr);
            if (!VarIs<RecyclableObject>(newTarget))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }

            RecyclableObject* newTargetObj = VarTo<RecyclableObject>(newTarget);
            Var proto = JavascriptOperators::GetPropertyNoCache(newTargetObj, PropertyIds::prototype, scriptContext);
            RecyclableObject* targetProto = nullptr;
            if (JavascriptOperators::IsObject(proto))
            {
                targetProto = VarTo<RecyclableObject>(proto);
            }
            else
            {
                ScriptContext* realmCtx = JavascriptOperators::GetFunctionRealm(newTargetObj, scriptContext);
                if (realmCtx == nullptr)
                {
                    realmCtx = scriptContext;
                }

                JavascriptLibrary* realmLib = realmCtx->GetLibrary();
                realmLib->EnsureIntlObjectReady();
                DynamicObject* defaultProto = (realmLib->*getProto)();
                if (defaultProto == nullptr)
                {
                    JavascriptLibrary* curLib = scriptContext->GetLibrary();
                    curLib->EnsureIntlObjectReady();
                    defaultProto = (curLib->*getProto)();
                }
                Assert(defaultProto != nullptr);
                targetProto = defaultProto;
            }

            if (CrossSite::NeedMarshalVar(targetProto, scriptContext))
            {
                Var marshaled = CrossSite::MarshalVar(scriptContext, targetProto);
                if (JavascriptOperators::IsObject(marshaled))
                {
                    targetProto = VarTo<RecyclableObject>(marshaled);
                }
            }
            return scriptContext->GetLibrary()->CreateObject(targetProto);
        }

        DynamicObject* RegisterIntlClass(
            JavascriptLibrary* library,
            DynamicObject* intlObject,
            const char16* className,
            FunctionInfo* ctorInfo,
            int length,
            const char16* toStringTag,
            DynamicObject* prototypeParent,
            RuntimeFunction** outCtor)
        {
            return IntlModernBuiltIns::RegisterClassImpl(
                library,
                intlObject,
                className,
                ctorInfo,
                length,
                toStringTag,
                prototypeParent,
                outCtor);
        }

        JavascriptFunction* AddIntlMethod(
            JavascriptLibrary* library,
            DynamicObject* obj,
            const char16* methodName,
            FunctionInfo* functionInfo,
            int length)
        {
            return IntlModernBuiltIns::AddMethodImpl(library, obj, methodName, functionInfo, length);
        }

        RuntimeFunction* AddIntlGetter(
            JavascriptLibrary* library,
            DynamicObject* obj,
            const char16* propName,
            FunctionInfo* functionInfo)
        {
            return IntlModernBuiltIns::AddGetterImpl(library, obj, propName, functionInfo);
        }

        DynamicObject* CreatePlainDataObject(ScriptContext* scriptContext)
        {
            Assert(scriptContext != nullptr);
            return scriptContext->GetLibrary()->CreateObject();
        }

        void SetDataProperty(DynamicObject* obj, PropertyId propId, Var value)
        {
            Assert(obj != nullptr && value != nullptr);
            obj->SetPropertyWithAttributes(propId, value, PropertyDynamicTypeDefaults, nullptr, PropertyOperation_None, SideEffects_None);
        }

        void SetDataProperty(DynamicObject* obj, const char16* propName, Var value, ScriptContext* scriptContext)
        {
            Assert(obj != nullptr && propName != nullptr && scriptContext != nullptr);
            PropertyId propId = scriptContext->GetOrAddPropertyIdTracked(propName, static_cast<charcount_t>(wcslen(propName)));
            SetDataProperty(obj, propId, value);
        }

        JavascriptString* CreateAsciiString(const char* ascii, size_t len, ScriptContext* scriptContext)
        {
            Assert(scriptContext != nullptr);
            if (len == 0 || ascii == nullptr)
            {
                return scriptContext->GetLibrary()->GetEmptyString();
            }
            if (len > INT32_MAX - 1)
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            charcount_t charLen = static_cast<charcount_t>(len);
            char16* buf = RecyclerNewArrayLeaf(scriptContext->GetRecycler(), char16, charLen + 1);
            for (charcount_t i = 0; i < charLen; ++i)
            {
                buf[i] = static_cast<char16>(static_cast<unsigned char>(ascii[i]));
            }
            buf[charLen] = _u('\0');
            return JavascriptString::NewWithBuffer(buf, charLen, scriptContext);
        }

        JavascriptString* CreateAsciiString(const char* ascii, ScriptContext* scriptContext)
        {
            return CreateAsciiString(ascii, ascii != nullptr ? strlen(ascii) : 0, scriptContext);
        }

        JavascriptString* CreateUtf16String(const UChar* utf16, int32_t len, ScriptContext* scriptContext)
        {
            Assert(scriptContext != nullptr);
            if (len <= 0 || utf16 == nullptr)
            {
                return scriptContext->GetLibrary()->GetEmptyString();
            }
            charcount_t charLen = static_cast<charcount_t>(len);
            char16* buf = RecyclerNewArrayLeaf(scriptContext->GetRecycler(), char16, charLen + 1);
            js_memcpy_s(buf, (charLen + 1) * sizeof(char16), utf16, charLen * sizeof(char16));
            buf[charLen] = _u('\0');
            return JavascriptString::NewWithBuffer(buf, charLen, scriptContext);
        }

        ScopedAsciiString::ScopedAsciiString() :
            buf(stackBuf),
            len(0),
            isAscii(true),
            oom(false)
        {
            stackBuf[0] = '\0';
        }

        ScopedAsciiString::~ScopedAsciiString()
        {
            if (buf != stackBuf)
            {
                NoCheckHeapDeleteArray(len + 1, buf);
            }
        }

        bool ScopedAsciiString::Init(JavascriptString* str)
        {
            Assert(str != nullptr);
            if (buf != stackBuf)
            {
                NoCheckHeapDeleteArray(len + 1, buf);
                buf = stackBuf;
            }
            len = str->GetLength();
            isAscii = true;
            oom = false;

            if (len + 1 > sizeof(stackBuf))
            {
                char* newBuf = NoCheckHeapNewArray(char, len + 1);
                if (newBuf == nullptr)
                {
                    oom = true;
                    len = 0;
                    stackBuf[0] = '\0';
                    return false;
                }
                buf = newBuf;
            }

            const char16* sz = str->GetString();
            for (size_t i = 0; i < len; ++i)
            {
                char16 ch = sz[i];
                if (ch > 0x7F)
                {
                    isAscii = false;
                    buf[i] = '\0';
                    return false;
                }
                buf[i] = static_cast<char>(ch);
            }
            buf[len] = '\0';
            return true;
        }

        void __declspec(noreturn) ThrowRangeErrorMessage(ScriptContext* scriptContext, const char16* message)
        {
            Assert(scriptContext != nullptr);
            const char16* safeMsg = (message != nullptr) ? message : _u("");
            size_t msgLen = wcslen(safeMsg);
            char16* gcCopy = RecyclerNewArrayLeaf(scriptContext->GetRecycler(), char16, msgLen + 1);
            js_memcpy_s(gcCopy, (msgLen + 1) * sizeof(char16), safeMsg, (msgLen + 1) * sizeof(char16));

            JavascriptError* pError = scriptContext->GetLibrary()->CreateRangeError();
            JavascriptError::SetErrorMessageProperties(pError, E_FAIL, gcCopy, scriptContext);
            JavascriptExceptionOperators::Throw(pError, scriptContext);
        }

        void __declspec(noreturn) ThrowLocaleRangeError(ScriptContext* scriptContext, JavascriptString* tag)
        {
            Assert(scriptContext != nullptr);
            const char16* tagStr = (tag != nullptr) ? tag->GetSz() : _u("");
            charcount_t tagLen = (tag != nullptr) ? tag->GetLength() : 0;

            const char16 prefix[] = _u("Locale '");
            const char16 suffix[] = _u("' is not well-formed");
            size_t prefixLen = _countof(prefix) - 1;
            size_t suffixLen = _countof(suffix) - 1;
            size_t totalLen = prefixLen + tagLen + suffixLen;

            char16* msg = RecyclerNewArrayLeaf(scriptContext->GetRecycler(), char16, totalLen + 1);
            js_memcpy_s(msg, (totalLen + 1) * sizeof(char16), prefix, prefixLen * sizeof(char16));
            if (tagLen > 0)
            {
                js_memcpy_s(msg + prefixLen, (totalLen + 1 - prefixLen) * sizeof(char16), tagStr, tagLen * sizeof(char16));
            }
            js_memcpy_s(msg + prefixLen + tagLen, (suffixLen + 1) * sizeof(char16), suffix, (suffixLen + 1) * sizeof(char16));

            JavascriptError* pError = scriptContext->GetLibrary()->CreateRangeError();
            JavascriptError::SetErrorMessageProperties(pError, E_FAIL, msg, scriptContext);
            JavascriptExceptionOperators::Throw(pError, scriptContext);
        }

        void __declspec(noreturn) ThrowOptionRangeError(
            ScriptContext* scriptContext,
            const char16* optionName,
            JavascriptString* optionValue,
            const char16* const* expectedValues,
            size_t expectedCount)
        {
            Assert(scriptContext != nullptr);
            CompoundString* cs = CompoundString::NewWithCharCapacity(128, scriptContext->GetLibrary());
            cs->Append(_u("Option value '"));
            if (optionValue != nullptr)
            {
                cs->Append(optionValue);
            }
            cs->Append(_u("' for '"));
            const char16* optSz = (optionName != nullptr) ? optionName : _u("");
            cs->Append(optSz, static_cast<charcount_t>(wcslen(optSz)));
            cs->Append(_u("' is outside of valid range. Expected: ["));
            for (size_t i = 0; i < expectedCount; ++i)
            {
                if (i > 0)
                {
                    cs->Append(_u(", "));
                }
                cs->Append(_u("'"));
                cs->Append(expectedValues[i], static_cast<charcount_t>(wcslen(expectedValues[i])));
                cs->Append(_u("'"));
            }
            cs->Append(_u("]"));

            const char16* sz = cs->GetSz();
            ThrowRangeErrorMessage(scriptContext, sz);
        }

        bool IsAsciiAlpha(char ch)
        {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        }

        bool IsAsciiDigit(char ch)
        {
            return ch >= '0' && ch <= '9';
        }

        bool IsAsciiAlnum(char ch)
        {
            return IsAsciiAlpha(ch) || IsAsciiDigit(ch);
        }

        char ToAsciiLower(char ch)
        {
            return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A')) : ch;
        }

        char ToAsciiUpper(char ch)
        {
            return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - ('a' - 'A')) : ch;
        }

        bool IsUnicodeLanguageSubtag(const char* sub, size_t len)
        {
            if (sub == nullptr)
            {
                return false;
            }
            if (!(len == 2 || len == 3 || (len >= 5 && len <= 8)))
            {
                return false;
            }
            return IsAllAlpha(sub, len);
        }

        bool IsUnicodeScriptSubtag(const char* sub, size_t len)
        {
            return sub != nullptr && len == 4 && IsAllAlpha(sub, len);
        }

        bool IsUnicodeRegionSubtag(const char* sub, size_t len)
        {
            if (sub == nullptr)
            {
                return false;
            }
            if (len == 2)
            {
                return IsAllAlpha(sub, 2);
            }
            if (len == 3)
            {
                return IsAllDigit(sub, 3);
            }
            return false;
        }

        bool IsUnicodeVariantSubtag(const char* sub, size_t len)
        {
            if (sub == nullptr)
            {
                return false;
            }
            if (len >= 5 && len <= 8)
            {
                return IsAllAlnum(sub, len);
            }
            if (len == 4)
            {
                return IsAsciiDigit(sub[0]) && IsAllAlnum(sub + 1, 3);
            }
            return false;
        }

        bool IsUnicodeTypeNonterminal(const char* str, size_t len)
        {
            SmallList<Subtag, 64> subtags;
            if (!SplitSubtags(str, len, subtags))
            {
                return false;
            }
            for (size_t i = 0; i < subtags.Count(); ++i)
            {
                if (subtags[i].len < 3 || subtags[i].len > 8)
                {
                    return false;
                }
            }
            return true;
        }

        bool IsValidUnicodeLanguageId(const char* tag, size_t len)
        {
            SmallList<Subtag, 64> subtags;
            if (!SplitSubtags(tag, len, subtags))
            {
                return false;
            }
            size_t pos = 0;
            return ParseUnicodeLanguageIdAt(tag, subtags, pos) && pos == subtags.Count();
        }

        bool IsStructurallyValidUnicodeLanguageTag(const char* tag, size_t len)
        {
            return ValidateUnicodeLanguageTagInternal(tag, len, nullptr);
        }

        bool TryCanonicalizeUnicodeLocaleId(
            const char* tag,
            size_t len,
            ScriptContext* scriptContext,
            JavascriptString** outCanonical)
        {
            Assert(scriptContext != nullptr && outCanonical != nullptr);
            *outCanonical = nullptr;

            bool oom = false;
            if (!ValidateUnicodeLanguageTagInternal(tag, len, &oom))
            {
                if (oom)
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                return false;
            }

            CharBuffer out;
            if (!CanonicalizeFullTag(tag, len, out, &oom))
            {
                if (oom)
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                return false;
            }

            *outCanonical = CreateAsciiString(out.Data(), out.Length(), scriptContext);
            return true;
        }

        JavascriptString* CanonicalizeUnicodeLocaleId(const char* tag, size_t len, ScriptContext* scriptContext)
        {
            JavascriptString* canonical = nullptr;
            if (!TryCanonicalizeUnicodeLocaleId(tag, len, scriptContext, &canonical))
            {
                JavascriptString* inputStr = CreateAsciiString(tag, len, scriptContext);
                ThrowLocaleRangeError(scriptContext, inputStr);
            }
            return canonical;
        }

        JavascriptString* ValidateAndCanonicalizeUnicodeLocaleId(JavascriptString* tag, ScriptContext* scriptContext)
        {
            Assert(tag != nullptr && scriptContext != nullptr);
            ScopedAsciiString ascii;
            if (!ascii.Init(tag))
            {
                if (ascii.IsOom())
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                ThrowLocaleRangeError(scriptContext, tag);
            }

            JavascriptString* canonical = nullptr;
            if (!TryCanonicalizeUnicodeLocaleId(ascii.Data(), ascii.Length(), scriptContext, &canonical))
            {
                ThrowLocaleRangeError(scriptContext, tag);
            }
            return canonical;
        }

        JavascriptString* MaximizeLocaleId(const char* canonicalTag, size_t len, ScriptContext* scriptContext)
        {
            Assert(canonicalTag != nullptr && scriptContext != nullptr);
            return TransformLikelySubtags(canonicalTag, len, true, scriptContext);
        }

        JavascriptString* MinimizeLocaleId(const char* canonicalTag, size_t len, ScriptContext* scriptContext)
        {
            Assert(canonicalTag != nullptr && scriptContext != nullptr);
            return TransformLikelySubtags(canonicalTag, len, false, scriptContext);
        }

        JavascriptArray* CanonicalizeLocaleList(Var locales, ScriptContext* scriptContext)
        {
            Assert(scriptContext != nullptr);
            JavascriptLibrary* library = scriptContext->GetLibrary();
            JavascriptArray* seen = library->CreateArray(0);
            if (JavascriptOperators::IsUndefined(locales))
            {
                return seen;
            }

            if (VarIs<JavascriptString>(locales))
            {
                JavascriptString* canonical = ValidateAndCanonicalizeUnicodeLocaleId(VarTo<JavascriptString>(locales), scriptContext);
                seen->DirectSetItemAt(0, canonical);
                return seen;
            }

            JavascriptString* directLocaleTag = TryGetLocaleTagIfLocaleObject(locales);
            if (directLocaleTag != nullptr)
            {
                JavascriptString* canonical = ValidateAndCanonicalizeUnicodeLocaleId(directLocaleTag, scriptContext);
                seen->DirectSetItemAt(0, canonical);
                return seen;
            }

            RecyclableObject* obj = JavascriptOperators::ToObject(locales, scriptContext);
            Var lenVar = JavascriptOperators::GetProperty(obj, PropertyIds::length, scriptContext);
            int64 lenSigned = JavascriptConversion::ToLength(lenVar, scriptContext);
            uint64 len = (lenSigned > 0) ? static_cast<uint64>(lenSigned) : 0;

            uint32 seenCount = 0;
            for (uint64 k = 0; k < len; ++k)
            {
                PropertyRecord const* propertyRecord = nullptr;
                JavascriptOperators::GetPropertyIdForInt(k, scriptContext, &propertyRecord);
                PropertyId propId = propertyRecord->GetPropertyId();

                if (!JavascriptOperators::HasProperty(obj, propId))
                {
                    continue;
                }

                Var kValue = JavascriptOperators::GetProperty(obj, propId, scriptContext);
                bool isString = VarIs<JavascriptString>(kValue) != FALSE;
                bool isObject = JavascriptOperators::IsObject(kValue) != FALSE;
                if (!isString && !isObject)
                {
                    JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
                }

                JavascriptString* tag = nullptr;
                if (!isString)
                {
                    tag = TryGetLocaleTagIfLocaleObject(kValue);
                }
                if (tag == nullptr)
                {
                    tag = JavascriptConversion::ToString(kValue, scriptContext);
                }

                JavascriptString* canonical = ValidateAndCanonicalizeUnicodeLocaleId(tag, scriptContext);
                bool duplicate = false;
                for (uint32 i = 0; i < seenCount; ++i)
                {
                    Var existingVar = seen->DirectGetItem(i);
                    if (JavascriptString::Equals(VarTo<JavascriptString>(existingVar), canonical))
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                {
                    seen->DirectSetItemAt(seenCount++, canonical);
                }
            }

            return seen;
        }

        RecyclableObject* GetOptionsObject(Var options, ScriptContext* scriptContext)
        {
            Assert(scriptContext != nullptr);
            if (JavascriptOperators::IsUndefined(options))
            {
                return nullptr;
            }
            if (!JavascriptOperators::IsObject(options))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }
            return VarTo<RecyclableObject>(options);
        }

        RecyclableObject* CoerceOptionsToObject(Var options, ScriptContext* scriptContext)
        {
            Assert(scriptContext != nullptr);
            if (JavascriptOperators::IsUndefined(options))
            {
                return nullptr;
            }
            return JavascriptOperators::ToObject(options, scriptContext);
        }

        JavascriptString* GetStringOption(
            RecyclableObject* options,
            PropertyId propId,
            const char16* optionName,
            const char16* const* values,
            size_t valueCount,
            const char16* fallback,
            ScriptContext* scriptContext)
        {
            Assert(scriptContext != nullptr);
            if (options == nullptr)
            {
                return (fallback != nullptr) ? JavascriptString::NewCopySz(fallback, scriptContext) : nullptr;
            }

            Var value = JavascriptOperators::GetProperty(options, propId, scriptContext);
            if (JavascriptOperators::IsUndefined(value))
            {
                return (fallback != nullptr) ? JavascriptString::NewCopySz(fallback, scriptContext) : nullptr;
            }

            JavascriptString* strValue = JavascriptConversion::ToString(value, scriptContext);
            if (values != nullptr && valueCount > 0)
            {
                const char16* actualSz = strValue->GetSz();
                charcount_t actualLen = strValue->GetLength();
                for (size_t i = 0; i < valueCount; ++i)
                {
                    const char16* allowed = values[i];
                    size_t allowedLen = wcslen(allowed);
                    if (actualLen == allowedLen && wmemcmp(actualSz, allowed, allowedLen) == 0)
                    {
                        return strValue;
                    }
                }
                ThrowOptionRangeError(scriptContext, optionName, strValue, values, valueCount);
            }

            return strValue;
        }

        JavascriptString* GetStringOption(
            RecyclableObject* options,
            const char16* optionName,
            const char16* const* values,
            size_t valueCount,
            const char16* fallback,
            ScriptContext* scriptContext)
        {
            Assert(optionName != nullptr && scriptContext != nullptr);
            if (options == nullptr)
            {
                return (fallback != nullptr) ? JavascriptString::NewCopySz(fallback, scriptContext) : nullptr;
            }
            PropertyId propId = scriptContext->GetOrAddPropertyIdTracked(optionName, static_cast<charcount_t>(wcslen(optionName)));
            return GetStringOption(options, propId, optionName, values, valueCount, fallback, scriptContext);
        }

        int GetBoolOption(RecyclableObject* options, PropertyId propId, int fallback, ScriptContext* scriptContext)
        {
            Assert(scriptContext != nullptr);
            if (options == nullptr)
            {
                return fallback;
            }
            Var value = JavascriptOperators::GetProperty(options, propId, scriptContext);
            if (JavascriptOperators::IsUndefined(value))
            {
                return fallback;
            }
            return JavascriptConversion::ToBoolean(value, scriptContext) ? 1 : 0;
        }

        int GetBoolOption(RecyclableObject* options, const char16* propName, int fallback, ScriptContext* scriptContext)
        {
            Assert(propName != nullptr && scriptContext != nullptr);
            if (options == nullptr)
            {
                return fallback;
            }
            PropertyId propId = scriptContext->GetOrAddPropertyIdTracked(propName, static_cast<charcount_t>(wcslen(propName)));
            return GetBoolOption(options, propId, fallback, scriptContext);
        }

        JavascriptString* GetDefaultLocale(ScriptContext* scriptContext)
        {
            Assert(scriptContext != nullptr);
            const char* icuDef = uloc_getDefault();
            if (icuDef != nullptr && icuDef[0] != '\0')
            {
                UErrorCode err = U_ZERO_ERROR;
                char tagBuf[ULOC_FULLNAME_CAPACITY] = { 0 };
                int32_t tagLen = uloc_toLanguageTag(icuDef, tagBuf, static_cast<int32_t>(sizeof(tagBuf)), FALSE, &err);
                if (U_SUCCESS(err) && tagLen > 0 && static_cast<size_t>(tagLen) < sizeof(tagBuf))
                {
                    JavascriptString* canonical = nullptr;
                    if (TryCanonicalizeUnicodeLocaleId(tagBuf, static_cast<size_t>(tagLen), scriptContext, &canonical))
                    {
                        return canonical;
                    }
                }
            }
            return CreateAsciiString("en-US", 5, scriptContext);
        }

        bool IsSupportedNumberingSystem(const char* nu, size_t len)
        {
            if (nu == nullptr || len == 0 || !IsUnicodeTypeNonterminal(nu, len))
            {
                return false;
            }

            char lowerNu[32];
            if (len >= sizeof(lowerNu))
            {
                return false;
            }
            for (size_t i = 0; i < len; ++i)
            {
                lowerNu[i] = ToAsciiLower(nu[i]);
            }
            lowerNu[len] = '\0';

            UErrorCode err = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUEnumeration names(unumsys_openAvailableNames(&err));
            if (U_FAILURE(err) || names == nullptr)
            {
                return false;
            }

            int32_t elemLen = 0;
            const char* elem = nullptr;
            while ((elem = uenum_next(names, &elemLen, &err)) != nullptr && U_SUCCESS(err))
            {
                if (elemLen == static_cast<int32_t>(len) && strncmp(elem, lowerNu, len) == 0)
                {
                    UErrorCode nsErr = U_ZERO_ERROR;
                    PlatformAgnostic::ICUHelpers::ScopedUNumberingSystem ns(unumsys_openByName(lowerNu, &nsErr));
                    if (U_SUCCESS(nsErr) && ns != nullptr && !unumsys_isAlgorithmic(ns))
                    {
                        return true;
                    }
                    return false;
                }
            }

            return false;
        }

        ResolvedLocale ResolveLocale(
            IntlModernKind kind,
            JavascriptArray* requestedLocales,
            const char16* /* localeMatcher */,
            const char16* const* relevantExtensionKeys,
            size_t keyCount,
            const char16* overrideNumberingSystem,
            ScriptContext* scriptContext)
        {
            Assert(scriptContext != nullptr);
            ResolvedLocale result = { nullptr, nullptr, nullptr };

            bool wantNu = false;
            for (size_t i = 0; i < keyCount; ++i)
            {
                if (relevantExtensionKeys != nullptr &&
                    relevantExtensionKeys[i] != nullptr &&
                    wcscmp(relevantExtensionKeys[i], _u("nu")) == 0)
                {
                    wantNu = true;
                    break;
                }
            }

            char matchedBase[ULOC_FULLNAME_CAPACITY] = { 0 };
            size_t matchedLen = 0;
            char requestedNu[32] = { 0 };
            bool hasRequestedNu = false;

            uint32 reqLen = (requestedLocales != nullptr) ? requestedLocales->GetLength() : 0;
            for (uint32 i = 0; i < reqLen; ++i)
            {
                Var itemVar = requestedLocales->DirectGetItem(i);
                if (!VarIs<JavascriptString>(itemVar))
                {
                    continue;
                }
                ScopedAsciiString ascii;
                if (!ascii.Init(VarTo<JavascriptString>(itemVar)))
                {
                    if (ascii.IsOom())
                    {
                        JavascriptError::ThrowOutOfMemoryError(scriptContext);
                    }
                    continue;
                }

                size_t baseLen = 0;
                if (!StripUnicodeAndPrivateExtensions(ascii.Data(), ascii.Length(), &baseLen))
                {
                    continue;
                }

                if (baseLen < sizeof(matchedBase) &&
                    BestAvailableLocale(kind, ascii.Data(), baseLen, matchedBase, sizeof(matchedBase), &matchedLen))
                {
                    if (wantNu)
                    {
                        hasRequestedNu = ExtractNumberingSystemFromTag(
                            ascii.Data(),
                            ascii.Length(),
                            requestedNu,
                            sizeof(requestedNu));
                    }
                    break;
                }
            }

            if (matchedLen == 0)
            {
                JavascriptString* defLocale = GetDefaultLocale(scriptContext);
                ScopedAsciiString defAscii;
                if (defAscii.Init(defLocale))
                {
                    size_t defBaseLen = 0;
                    if (StripUnicodeAndPrivateExtensions(defAscii.Data(), defAscii.Length(), &defBaseLen))
                    {
                        if (!BestAvailableLocale(kind, defAscii.Data(), defBaseLen, matchedBase, sizeof(matchedBase), &matchedLen))
                        {
                            js_memcpy_s(matchedBase, sizeof(matchedBase), "en", 3);
                            matchedLen = 2;
                        }
                    }
                }
                if (matchedLen == 0)
                {
                    js_memcpy_s(matchedBase, sizeof(matchedBase), "en", 3);
                    matchedLen = 2;
                }
            }

            result.dataLocale = CreateAsciiString(matchedBase, matchedLen, scriptContext);

            if (!wantNu)
            {
                result.locale = result.dataLocale;
                return result;
            }

            char icuBase[ULOC_FULLNAME_CAPACITY] = { 0 };
            for (size_t i = 0; i < matchedLen && i < sizeof(icuBase) - 1; ++i)
            {
                icuBase[i] = (matchedBase[i] == '-') ? '_' : matchedBase[i];
            }
            icuBase[matchedLen < sizeof(icuBase) ? matchedLen : sizeof(icuBase) - 1] = '\0';

            char defaultNu[32] = { 0 };
            GetDefaultNumberingSystemForLocale(icuBase, defaultNu, sizeof(defaultNu));

            char chosenNu[32] = { 0 };
            bool appendNuExt = false;

            if (overrideNumberingSystem != nullptr && overrideNumberingSystem[0] != _u('\0'))
            {
                size_t ovLen = wcslen(overrideNumberingSystem);
                if (ovLen < sizeof(chosenNu))
                {
                    bool asciiOk = true;
                    for (size_t i = 0; i < ovLen; ++i)
                    {
                        char16 ch = overrideNumberingSystem[i];
                        if (ch > 0x7F)
                        {
                            asciiOk = false;
                            break;
                        }
                        chosenNu[i] = ToAsciiLower(static_cast<char>(ch));
                    }
                    chosenNu[ovLen] = '\0';
                    if (asciiOk && IsSupportedNumberingSystem(chosenNu, ovLen))
                    {
                        appendNuExt = (strcmp(chosenNu, defaultNu) != 0);
                    }
                    else
                    {
                        chosenNu[0] = '\0';
                    }
                }
            }

            if (chosenNu[0] == '\0' && hasRequestedNu)
            {
                size_t reqNuLen = strlen(requestedNu);
                if (IsSupportedNumberingSystem(requestedNu, reqNuLen))
                {
                    js_memcpy_s(chosenNu, sizeof(chosenNu), requestedNu, reqNuLen + 1);
                    appendNuExt = (strcmp(chosenNu, defaultNu) != 0);
                }
            }

            if (chosenNu[0] == '\0')
            {
                js_memcpy_s(chosenNu, sizeof(chosenNu), defaultNu, strlen(defaultNu) + 1);
                appendNuExt = false;
            }

            result.numberingSystem = CreateAsciiString(chosenNu, scriptContext);
            if (appendNuExt)
            {
                char fullTag[ULOC_FULLNAME_CAPACITY + 40];
                snprintf(fullTag, sizeof(fullTag), "%s-u-nu-%s", matchedBase, chosenNu);
                result.locale = CreateAsciiString(fullTag, scriptContext);
            }
            else
            {
                result.locale = result.dataLocale;
            }

            return result;
        }

        JavascriptArray* SupportedLocales(
            IntlModernKind kind,
            Var locales,
            Var options,
            ScriptContext* scriptContext)
        {
            Assert(scriptContext != nullptr);
            JavascriptArray* requested = CanonicalizeLocaleList(locales, scriptContext);
            RecyclableObject* optObj = CoerceOptionsToObject(options, scriptContext);
            static const char16* const matcherValues[] = { _u("lookup"), _u("best fit") };
            (void)GetStringOption(
                optObj,
                PropertyIds::localeMatcher,
                _u("localeMatcher"),
                matcherValues,
                _countof(matcherValues),
                _u("best fit"),
                scriptContext);

            JavascriptArray* supported = scriptContext->GetLibrary()->CreateArray(0);
            uint32 reqLen = requested->GetLength();
            uint32 outCount = 0;

            for (uint32 i = 0; i < reqLen; ++i)
            {
                Var itemVar = requested->DirectGetItem(i);
                JavascriptString* locStr = VarTo<JavascriptString>(itemVar);
                ScopedAsciiString ascii;
                if (!ascii.Init(locStr))
                {
                    if (ascii.IsOom())
                    {
                        JavascriptError::ThrowOutOfMemoryError(scriptContext);
                    }
                    continue;
                }

                size_t baseLen = 0;
                if (!StripUnicodeAndPrivateExtensions(ascii.Data(), ascii.Length(), &baseLen))
                {
                    continue;
                }

                char matched[ULOC_FULLNAME_CAPACITY] = { 0 };
                size_t matchedLen = 0;
                if (baseLen < sizeof(matched) &&
                    BestAvailableLocale(kind, ascii.Data(), baseLen, matched, sizeof(matched), &matchedLen))
                {
                    supported->DirectSetItemAt(outCount++, locStr);
                }
            }

            return supported;
        }
    } // namespace IntlModern

    DynamicObject* IntlModernBuiltIns::RegisterClassImpl(
        JavascriptLibrary* library,
        DynamicObject* intlObject,
        const char16* className,
        FunctionInfo* ctorInfo,
        int length,
        const char16* toStringTag,
        DynamicObject* prototypeParent,
        RuntimeFunction** outCtor)
    {
        Assert(library != nullptr && intlObject != nullptr && className != nullptr && ctorInfo != nullptr);
        ScriptContext* scriptContext = library->GetScriptContext();

        DynamicObject* proto = (prototypeParent != nullptr)
            ? library->CreateObject(prototypeParent)
            : library->CreateObject();

        if (toStringTag != nullptr)
        {
            JavascriptString* tagStr = JavascriptString::NewCopySz(toStringTag, scriptContext);
            library->AddMember(proto, PropertyIds::_symbolToStringTag, tagStr, PropertyConfigurable);
        }

        PropertyId classPid = scriptContext->GetOrAddPropertyIdTracked(className, static_cast<charcount_t>(wcslen(className)));
        RuntimeFunction* ctor = library->DefaultCreateFunction(ctorInfo, length, proto, nullptr, classPid);
        library->AddMember(proto, PropertyIds::constructor, ctor, PropertyWritable | PropertyConfigurable);
        library->AddMember(intlObject, classPid, ctor, PropertyBuiltInMethodDefaults);

        if (outCtor != nullptr)
        {
            *outCtor = ctor;
        }
        return proto;
    }

    JavascriptFunction* IntlModernBuiltIns::AddMethodImpl(
        JavascriptLibrary* library,
        DynamicObject* obj,
        const char16* methodName,
        FunctionInfo* functionInfo,
        int length)
    {
        Assert(library != nullptr && obj != nullptr && methodName != nullptr && functionInfo != nullptr);
        ScriptContext* scriptContext = library->GetScriptContext();
        PropertyId pid = scriptContext->GetOrAddPropertyIdTracked(methodName, static_cast<charcount_t>(wcslen(methodName)));
        return library->AddFunctionToLibraryObject(obj, pid, functionInfo, length);
    }

    RuntimeFunction* IntlModernBuiltIns::AddGetterImpl(
        JavascriptLibrary* library,
        DynamicObject* obj,
        const char16* propName,
        FunctionInfo* functionInfo)
    {
        Assert(library != nullptr && obj != nullptr && propName != nullptr && functionInfo != nullptr);
        ScriptContext* scriptContext = library->GetScriptContext();
        PropertyId pid = scriptContext->GetOrAddPropertyIdTracked(propName, static_cast<charcount_t>(wcslen(propName)));
        return library->AddGetterToLibraryObject(obj, pid, functionInfo);
    }

    Var IntlModernBuiltIns::EntryGetCanonicalLocales(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.getCanonicalLocales"));
        Assert(!(callInfo.Flags & CallFlags_New));

        Var locales = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        return IntlModern::CanonicalizeLocaleList(locales, scriptContext);
    }
} // namespace Js

#endif // INTL_ICU
