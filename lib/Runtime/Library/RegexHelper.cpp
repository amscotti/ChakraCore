//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

// Parser Includes
#include "DebugWriter.h"
#include "RegexStats.h"
#include "OctoquadIdentifier.h"
#include "RegexCompileTime.h"
#include "RegexParser.h"
#include "RegexPattern.h"

namespace Js
{
    // ----------------------------------------------------------------------
    // Dynamic compilation
    // ----------------------------------------------------------------------

    // See also:
    //    UnifiedRegex::Parser::Options(...)
    bool RegexHelper::GetFlags(Js::ScriptContext* scriptContext, __in_ecount(strLen) const char16* str, CharCount strLen, UnifiedRegex::RegexFlags &flags)
    {
        for (CharCount i = 0; i < strLen; i++)
        {
            switch (str[i])
            {
            case 'd':
                // ES2022 'd' flag: unconditional core syntax (no config flag)
                if ((flags & UnifiedRegex::HasIndicesRegexFlag) != 0)
                    return false;
                flags = (UnifiedRegex::RegexFlags)(flags | UnifiedRegex::HasIndicesRegexFlag);
                break;
            case 'i':
                if ((flags & UnifiedRegex::IgnoreCaseRegexFlag) != 0)
                    return false;
                flags = (UnifiedRegex::RegexFlags)(flags | UnifiedRegex::IgnoreCaseRegexFlag);
                break;
            case 'g':
                if ((flags & UnifiedRegex::GlobalRegexFlag) != 0)
                    return false;
                flags = (UnifiedRegex::RegexFlags)(flags | UnifiedRegex::GlobalRegexFlag);
                break;
            case 'm':
                if ((flags & UnifiedRegex::MultilineRegexFlag) != 0)
                    return false;
                flags = (UnifiedRegex::RegexFlags)(flags | UnifiedRegex::MultilineRegexFlag);
                break;
            case 's':
                if (scriptContext->GetConfig()->IsES2018RegExDotAllEnabled())
                {
                    if ((flags & UnifiedRegex::DotAllRegexFlag) != 0)
                        return false;
                    flags = (UnifiedRegex::RegexFlags)(flags | UnifiedRegex::DotAllRegexFlag);
                    break;
                }
                return false;
            case 'u':
                if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                {
                    if((flags & UnifiedRegex::UnicodeRegexFlag) != 0)
                        return false;
                    // ES2024: 'u' and 'v' are mutually exclusive.
                    if ((flags & UnifiedRegex::UnicodeSetsRegexFlag) != 0)
                        return false;
                    flags = (UnifiedRegex::RegexFlags)(flags | UnifiedRegex::UnicodeRegexFlag);
                    break;
                }
                return false;
            case 'v':
                // ES2024 'v' flag (Unicode sets), gated like 'u'.
                if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                {
                    if ((flags & UnifiedRegex::UnicodeSetsRegexFlag) != 0)
                        return false;
                    if ((flags & UnifiedRegex::UnicodeRegexFlag) != 0)
                        return false;
                    flags = (UnifiedRegex::RegexFlags)(flags | UnifiedRegex::UnicodeSetsRegexFlag);
                    break;
                }
                return false;
            case 'y':
                if (scriptContext->GetConfig()->IsES6RegExStickyEnabled())
                {
                    if ((flags & UnifiedRegex::StickyRegexFlag) != 0)
                        return false;
                    flags = (UnifiedRegex::RegexFlags)(flags | UnifiedRegex::StickyRegexFlag);
                    break;
                }
                return false;
            default:
                return false;
            }
        }

        return true;
    }

    UnifiedRegex::RegexPattern* RegexHelper::CompileDynamic(ScriptContext *scriptContext, const char16* psz, CharCount csz, const char16* pszOpts, CharCount cszOpts, bool isLiteralSource)
    {
        Assert(psz != 0 && psz[csz] == 0);
        Assert(pszOpts != 0 || cszOpts == 0);
        Assert(pszOpts == 0 || pszOpts[cszOpts] == 0);

        UnifiedRegex::RegexFlags flags = UnifiedRegex::NoRegexFlags;

        if (pszOpts != NULL)
        {
            if (!GetFlags(scriptContext, pszOpts, cszOpts, flags))
            {
                // Compile in order to throw appropriate error for ill-formed flags
                PrimCompileDynamic(scriptContext, psz, csz, pszOpts, cszOpts, isLiteralSource);
                Assert(false);
            }
        }

        if(isLiteralSource)
        {
            // The source is from a literal regex, so we're cloning a literal regex. Don't use the dynamic regex MRU map since
            // these literal regex patterns' lifetimes are tied with the function body.
            return PrimCompileDynamic(scriptContext, psz, csz, pszOpts, cszOpts, isLiteralSource);
        }

        UnifiedRegex::RegexKey lookupKey(psz, csz, flags);
        UnifiedRegex::RegexPattern* pattern = nullptr;
        RegexPatternMruMap* dynamicRegexMap = scriptContext->GetDynamicRegexMap();
        if (!dynamicRegexMap->TryGetValue(lookupKey, &pattern))
        {
            pattern = PrimCompileDynamic(scriptContext, psz, csz, pszOpts, cszOpts, isLiteralSource);

            // WARNING: Must calculate key again so that dictionary has copy of source associated with the pattern
            const auto source = pattern->GetSource();
            UnifiedRegex::RegexKey finalKey(source.GetBuffer(), source.GetLength(), flags);
            dynamicRegexMap->Add(finalKey, pattern);
        }
        return pattern;
    }

    UnifiedRegex::RegexPattern* RegexHelper::CompileDynamic(
        ScriptContext *scriptContext, const char16* psz, CharCount csz, UnifiedRegex::RegexFlags flags, bool isLiteralSource)
    {
        //
        // Regex compilations are mostly string parsing based. To avoid duplicating validation rules,
        // generate a trivial options string right here on the stack and delegate to the string parsing
        // based implementation.
        //
        // dgimsuy plus a trailing NUL. Seven flags no longer fit in 7 char16s.
        const CharCount OPT_BUF_SIZE = 10;
        char16 opts[OPT_BUF_SIZE];

        CharCount i = 0;
        if (flags & UnifiedRegex::HasIndicesRegexFlag)
        {
            opts[i++] = _u('d');
        }
        if (flags & UnifiedRegex::IgnoreCaseRegexFlag)
        {
            opts[i++] = _u('i');
        }
        if (flags & UnifiedRegex::GlobalRegexFlag)
        {
            opts[i++] = _u('g');
        }
        if (flags & UnifiedRegex::MultilineRegexFlag)
        {
            opts[i++] = _u('m');
        }
        if (flags & UnifiedRegex::DotAllRegexFlag)
        {
            Assert(scriptContext->GetConfig()->IsES2018RegExDotAllEnabled());
            opts[i++] = _u('s');
        }
        if (flags & UnifiedRegex::UnicodeRegexFlag)
        {
            Assert(scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled());
            opts[i++] = _u('u');
        }
        if (flags & UnifiedRegex::UnicodeSetsRegexFlag)
        {
            Assert(scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled());
            opts[i++] = _u('v');
        }
        if (flags & UnifiedRegex::StickyRegexFlag)
        {
            Assert(scriptContext->GetConfig()->IsES6RegExStickyEnabled());
            opts[i++] = _u('y');
        }
        Assert(i < OPT_BUF_SIZE);
        opts[i] = NULL;

        return CompileDynamic(scriptContext, psz, csz, opts, i, isLiteralSource);
    }

    UnifiedRegex::RegexPattern* RegexHelper::PrimCompileDynamic(ScriptContext *scriptContext, const char16* psz, CharCount csz, const char16* pszOpts, CharCount cszOpts, bool isLiteralSource)
    {
        PROBE_STACK_NO_DISPOSE(scriptContext, Js::Constants::MinStackRegex);

        // SEE ALSO: Scanner<EncodingPolicy>::ScanRegExpConstant()
#ifdef PROFILE_EXEC
        scriptContext->ProfileBegin(Js::RegexCompilePhase);
#endif
        ArenaAllocator* rtAllocator = scriptContext->RegexAllocator();
#if ENABLE_REGEX_CONFIG_OPTIONS
        UnifiedRegex::DebugWriter *dw = 0;
        if (REGEX_CONFIG_FLAG(RegexDebug))
            dw = scriptContext->GetRegexDebugWriter();
        UnifiedRegex::RegexStats* stats = 0;
#endif
        UnifiedRegex::RegexFlags flags = UnifiedRegex::NoRegexFlags;

        if(csz == 0 && cszOpts == 0)
        {
            // Fast path for compiling the empty regex with empty flags, for the RegExp constructor object and other cases.
            // These empty regexes are dynamic regexes and so this fast path only exists for dynamic regex compilation. The
            // standard chars in particular, do not need to be initialized to compile this regex.
            UnifiedRegex::Program* program = UnifiedRegex::Program::New(scriptContext->GetRecycler(), flags);
            UnifiedRegex::Parser<NullTerminatedUnicodeEncodingPolicy, false>::CaptureEmptySourceAndNoGroups(program);
            UnifiedRegex::RegexPattern* pattern = UnifiedRegex::RegexPattern::New(scriptContext, program, false);
            UnifiedRegex::Compiler::CompileEmptyRegex
                ( program
                , pattern
#if ENABLE_REGEX_CONFIG_OPTIONS
                , dw
                , stats
#endif
                );
#ifdef PROFILE_EXEC
            scriptContext->ProfileEnd(Js::RegexCompilePhase);
#endif
            return pattern;
        }

#if ENABLE_REGEX_CONFIG_OPTIONS
        if (REGEX_CONFIG_FLAG(RegexProfile))
            scriptContext->GetRegexStatsDatabase()->BeginProfile();
#endif
        BEGIN_TEMP_ALLOCATOR(ctAllocator, scriptContext, _u("UnifiedRegexParseAndCompile"));
        UnifiedRegex::StandardChars<char16>* standardChars = scriptContext->GetThreadContext()->GetStandardChars((char16*)0);
        UnifiedRegex::Node* root = 0;
        UnifiedRegex::Parser<NullTerminatedUnicodeEncodingPolicy, false> parser
            ( scriptContext
            , ctAllocator
            , standardChars
            , standardChars
            , false
#if ENABLE_REGEX_CONFIG_OPTIONS
            , dw
#endif
            );
        try
        {
            root = parser.ParseDynamic(psz, psz + csz, pszOpts, pszOpts + cszOpts, flags);
        }
        catch (UnifiedRegex::ParseError e)
        {
            END_TEMP_ALLOCATOR(ctAllocator, scriptContext);
#ifdef PROFILE_EXEC
            scriptContext->ProfileEnd(Js::RegexCompilePhase);
#endif
            Js::JavascriptError::ThrowSyntaxError(scriptContext, e.error);
            // never reached
        }

        const auto recycler = scriptContext->GetRecycler();
        UnifiedRegex::Program* program = UnifiedRegex::Program::New(recycler, flags);
        parser.CaptureSourceAndGroups(recycler, program, psz, csz, csz);

        UnifiedRegex::RegexPattern* pattern = UnifiedRegex::RegexPattern::New(scriptContext, program, isLiteralSource);

#if ENABLE_REGEX_CONFIG_OPTIONS
        if (REGEX_CONFIG_FLAG(RegexProfile))
        {
            stats = scriptContext->GetRegexStatsDatabase()->GetRegexStats(pattern);
            scriptContext->GetRegexStatsDatabase()->EndProfile(stats, UnifiedRegex::RegexStats::Parse);
        }
        if (REGEX_CONFIG_FLAG(RegexTracing))
        {
            UnifiedRegex::DebugWriter* tw = scriptContext->GetRegexDebugWriter();
            tw->Print(_u("// REGEX COMPILE "));
            pattern->Print(tw);
            tw->EOL();
        }
        if (REGEX_CONFIG_FLAG(RegexProfile))
            scriptContext->GetRegexStatsDatabase()->BeginProfile();
#endif

        UnifiedRegex::Compiler::Compile
            ( scriptContext
            , ctAllocator
            , rtAllocator
            , standardChars
            , program
            , root
            , parser.GetLitbuf()
            , pattern
#if ENABLE_REGEX_CONFIG_OPTIONS
            , dw
            , stats
#endif
            );

#if ENABLE_REGEX_CONFIG_OPTIONS
        if (REGEX_CONFIG_FLAG(RegexProfile))
            scriptContext->GetRegexStatsDatabase()->EndProfile(stats, UnifiedRegex::RegexStats::Compile);
#endif

        END_TEMP_ALLOCATOR(ctAllocator, scriptContext);
#ifdef PROFILE_EXEC
        scriptContext->ProfileEnd(Js::RegexCompilePhase);
#endif

        return pattern;

    }

    // ----------------------------------------------------------------------
    // Primitives
    // ----------------------------------------------------------------------
#if ENABLE_REGEX_CONFIG_OPTIONS


    static void RegexHelperTrace(
        ScriptContext* scriptContext,
        UnifiedRegex::RegexStats::Use use,
        JavascriptRegExp* regExp,
        const char16 *const input,
        const CharCount inputLength,
        const char16 *const replace = 0,
        const CharCount replaceLength = 0)
    {
        Assert(regExp);
        Assert(input);

        if (REGEX_CONFIG_FLAG(RegexProfile))
        {
            UnifiedRegex::RegexStats* stats =
                scriptContext->GetRegexStatsDatabase()->GetRegexStats(regExp->GetPattern());
            stats->useCounts[use]++;
            stats->inputLength += inputLength;
        }
        if (REGEX_CONFIG_FLAG(RegexTracing))
        {
            UnifiedRegex::DebugWriter* w = scriptContext->GetRegexDebugWriter();
            w->Print(_u("%s("), UnifiedRegex::RegexStats::UseNames[use]);
            regExp->GetPattern()->Print(w);
            w->Print(_u(", "));
            if (!CONFIG_FLAG(Verbose) && inputLength > 1024)
                w->Print(_u("\"<string too large>\""));
            else
                w->PrintQuotedString(input, inputLength);
            if (replace != 0)
            {
                Assert(use == UnifiedRegex::RegexStats::Replace);
                w->Print(_u(", "));
                if (!CONFIG_FLAG(Verbose) && replaceLength > 1024)
                    w->Print(_u("\"<string too large>\""));
                else
                    w->PrintQuotedString(replace, replaceLength);
            }
            w->PrintEOL(_u(");"));
            w->Flush();
        }
    }

    static void RegexHelperTrace(ScriptContext* scriptContext, UnifiedRegex::RegexStats::Use use, JavascriptRegExp* regExp, JavascriptString* input)
    {
        Assert(regExp);
        Assert(input);

        RegexHelperTrace(scriptContext, use, regExp, input->GetString(), input->GetLength());
    }

    static void RegexHelperTrace(ScriptContext* scriptContext, UnifiedRegex::RegexStats::Use use, JavascriptRegExp* regExp, JavascriptString* input, JavascriptString* replace)
    {
        Assert(regExp);
        Assert(input);
        Assert(replace);

        RegexHelperTrace(scriptContext, use, regExp, input->GetString(), input->GetLength(), replace->GetString(), replace->GetLength());
    }
#endif

    // ----------------------------------------------------------------------
    // Regex entry points
    // ----------------------------------------------------------------------

    struct RegexMatchState
    {
        const char16* input;
        TempArenaAllocatorObject* tempAllocatorObj;
        UnifiedRegex::Matcher* matcher;
    };

    // ----------------------------------------------------------------------
    // u/v code-point walk helpers.
    //
    // Matcher::Match (RegexRuntime.cpp) never starts an attempt at a
    // position that splits a complete surrogate pair: an offset whose
    // previous unit is a lead surrogate and whose unit is a trail surrogate
    // resolves to the containing code point (ES2026 22.2.7.2 step 13.b).
    // The helpers below keep every internal global walk feeding such a
    // position to the next attempt: an empty match advances per code point
    // (AdvanceStringIndex), and the engine's documented lead-half
    // pair-lacunarity (a one-unit match of a lead surrogate whose trail
    // half follows) steps past the trail half so the walk cannot ping-pong
    // through the pair-start snap. Non-u keeps the per-unit Annex B
    // advance exactly.
    // ----------------------------------------------------------------------

    static bool RegexSplitsSurrogatePairAt(const char16* input, CharCount inputLength, CharCount position)
    {
        return position > 0 && position < inputLength
            && Js::NumberUtilities::IsSurrogateUpperPart(input[position])
            && Js::NumberUtilities::IsSurrogateLowerPart(input[position - 1]);
    }

    static CharCount RegexAdvanceStringIndexUnicode(const char16* input, CharCount inputLength, CharCount index)
    {
        if (index + 1 >= inputLength)
        {
            return index + 1;
        }
        if (Js::NumberUtilities::IsSurrogateLowerPart(input[index])
            && Js::NumberUtilities::IsSurrogateUpperPart(input[index + 1]))
        {
            return index + 2;
        }
        return index + 1;
    }

    static bool RegexIsUnicodePattern(UnifiedRegex::RegexPattern* pattern)
    {
        return pattern->IsUnicode() || pattern->IsUnicodeSets();
    }

    // Next start after a successful u/v match [matchStart, matchEnd).
    static CharCount RegexNextMatchStart(const char16* input, CharCount inputLength, CharCount matchStart, CharCount matchEnd)
    {
        if (matchEnd == matchStart)
        {
            return RegexAdvanceStringIndexUnicode(input, inputLength, matchStart);
        }
        if (matchEnd < inputLength
            && Js::NumberUtilities::IsSurrogateUpperPart(input[matchEnd])
            && Js::NumberUtilities::IsSurrogateLowerPart(input[matchEnd - 1])
            && matchEnd - 1 <= matchStart)
        {
            return matchEnd + 1;
        }
        return matchEnd;
    }

    // The match reported to PropagateLastMatch for the regex's "lastIndex"
    // write after a successful exec/test. A one-unit u/v match ending on a
    // pair's second half is the engine's documented lead-half lacunarity
    // (Node ends u matches only on code-point boundaries); writing the raw
    // end would make every exec/test loop re-enter the same match through
    // the pair-start snap in Matcher::Match, so step past the trail half.
    // The result array / RegExp ctor keep the unadjusted match.
    static UnifiedRegex::GroupInfo RegexAdjustedLastMatchForPropagation(
        UnifiedRegex::RegexPattern* pattern,
        const char16* input,
        CharCount inputLength,
        const UnifiedRegex::GroupInfo& match)
    {
        if (!match.IsUndefined() && RegexIsUnicodePattern(pattern) && match.length == 1)
        {
            const CharCount end = match.EndOffset();
            if (RegexSplitsSurrogatePairAt(input, inputLength, end))
            {
                UnifiedRegex::GroupInfo adjusted = match;
                adjusted.length = 2;
                return adjusted;
            }
        }
        return match;
    }

    template <bool updateHistory>
    Var RegexHelper::RegexMatchImpl(ScriptContext* scriptContext, RecyclableObject *thisObj, JavascriptString *input, bool noResult, void *const stackAllocationPointer)
    {
        ScriptConfiguration const * scriptConfig = scriptContext->GetConfig();

        // Normally, this check would be done in JavascriptRegExp::EntrySymbolMatch. However,
        // since the lowerer inlines String.prototype.match and directly calls the helper,
        // the check then would be bypassed. That's the reason we do the check here.
        if (scriptConfig->IsES6RegExSymbolsEnabled()
            && IsRegexSymbolMatchObservable(thisObj, scriptContext))
        {
            // We don't need to pass "updateHistory" here since the call to "exec" will handle it.
            return RegexEs6MatchImpl(scriptContext, thisObj, input, noResult, stackAllocationPointer);
        }
        else
        {
            PCWSTR varName = scriptConfig->IsES6RegExSymbolsEnabled()
                ? _u("RegExp.prototype[Symbol.match]")
                : _u("String.prototype.match");
            JavascriptRegExp* regularExpression = JavascriptRegExp::ToRegExp(thisObj, varName, scriptContext);
            return RegexEs5MatchImpl<updateHistory>(scriptContext, regularExpression, input, noResult, stackAllocationPointer);
        }
    }

    bool RegexHelper::IsRegexSymbolMatchObservable(RecyclableObject* instance, ScriptContext* scriptContext)
    {
        DynamicObject* regexPrototype = scriptContext->GetLibrary()->GetRegExpPrototype();
        return !JavascriptRegExp::HasOriginalRegExType(instance)
            || JavascriptRegExp::HasObservableExec(regexPrototype)
            || JavascriptRegExp::HasObservableGlobalFlag(regexPrototype)
            || JavascriptRegExp::HasObservableUnicodeFlag(regexPrototype);
    }

    Var RegexHelper::RegexEs6MatchImpl(ScriptContext* scriptContext, RecyclableObject *thisObj, JavascriptString *input, bool noResult, void *const stackAllocationPointer)
    {
        PCWSTR const varName = _u("RegExp.prototype[Symbol.match]");

        // ES2026 @@match: global and fullUnicode ('u' OR 'v') derive from ONE
        // observable Get(R, "flags") + ToString (substrate fallback for a
        // genuine RegExp whose "flags" accessor was not installed — the same
        // fallback @@matchAll makes; a user-provided value is never ignored).
        bool global = false;
        bool fullUnicode = false;
        JavascriptRegExp::ParseBuiltinFlags(
            JavascriptRegExp::GetFlagsStringForBuiltin(thisObj, scriptContext), global, fullUnicode);

        if (!global)
        {
            return JavascriptRegExp::CallExec(thisObj, input, varName, scriptContext);
        }
        else
        {
            JavascriptRegExp::SetLastIndexProperty(thisObj, TaggedInt::ToVarUnchecked(0), scriptContext);

            JavascriptArray* arrayResult = nullptr;

            do
            {
                Var result = JavascriptRegExp::CallExec(thisObj, input, varName, scriptContext);
                if (JavascriptOperators::IsNull(result))
                {
                    break;
                }

                RecyclableObject* resultObj = ExecResultToRecyclableObject(result);
                JavascriptString* matchStr = GetMatchStrFromResult(resultObj, scriptContext);

                if (arrayResult == nullptr)
                {
                    arrayResult = scriptContext->GetLibrary()->CreateArray();
                }

                arrayResult->DirectAppendItem(matchStr);

                AdvanceLastIndex(thisObj, input, matchStr, fullUnicode, scriptContext);
            }
            while (true);

            return arrayResult != nullptr
                ? arrayResult
                : scriptContext->GetLibrary()->GetNull();
        }
    }

    // String.prototype.match (ES5 15.5.4.10)
    template <bool updateHistory>
    Var RegexHelper::RegexEs5MatchImpl(ScriptContext* scriptContext, JavascriptRegExp *regularExpression, JavascriptString *input, bool noResult, void *const stackAllocationPointer)
    {
        UnifiedRegex::RegexPattern* pattern = regularExpression->GetPattern();
        const char16* inputStr = input->GetString();
        CharCount inputLength = input->GetLength();

#if ENABLE_REGEX_CONFIG_OPTIONS
        RegexHelperTrace(scriptContext, UnifiedRegex::RegexStats::Match, regularExpression, input);
#endif

        UnifiedRegex::GroupInfo lastSuccessfulMatch; // initially undefined
        UnifiedRegex::GroupInfo lastActualMatch; // initially undefined

#ifdef REGEX_TRIGRAMS
        UnifiedRegex::TrigramAlphabet* trigramAlphabet = scriptContext->GetTrigramAlphabet();
        UnifiedRegex::TrigramInfo* trigramInfo = pattern->rep.unified.trigramInfo;
        if (trigramAlphabet != NULL && inputLength >= MinTrigramInputLength && trigramInfo != NULL)
        {
            if (trigramAlphabet->input == NULL)
            {
                trigramAlphabet->MegaMatch((char16*)inputStr, inputLength);
            }
            if (trigramInfo->isTrigramPattern)
            {
                if (trigramInfo->resultCount > 0)
                {
                    lastSuccessfulMatch.offset = trigramInfo->offsets[trigramInfo->resultCount - 1];
                    lastSuccessfulMatch.length = UnifiedRegex::TrigramInfo::PatternLength;
                }
                // else: leave lastMatch undefined

                // Make sure a matcher is allocated and holds valid last match in case the RegExp constructor
                // needs to fill-in details from the last match via JavascriptRegExpConstructor::EnsureValues
                Assert(pattern->rep.unified.program != 0);
                if (pattern->rep.unified.matcher == 0)
                    pattern->rep.unified.matcher = UnifiedRegex::Matcher::New(scriptContext, pattern);
                *pattern->rep.unified.matcher->GroupIdToGroupInfo(0) = lastSuccessfulMatch;

                Assert(pattern->IsGlobal());

                JavascriptArray* arrayResult = CreateMatchResult(stackAllocationPointer, scriptContext, /* isGlobal */ true, pattern->NumGroups(), input, /* hasIndices */ false);
                FinalizeMatchResult(scriptContext, /* isGlobal */ true, arrayResult, lastSuccessfulMatch);

                if (trigramInfo->resultCount > 0)
                {
                    if (trigramInfo->hasCachedResultString)
                    {
                        for (int k = 0; k < trigramInfo->resultCount; k++)
                        {
                            arrayResult->DirectSetItemAt(k,
                                static_cast<Js::JavascriptString*>(trigramInfo->cachedResult[k]));
                        }
                    }
                    else
                    {
                        for (int k = 0; k < trigramInfo->resultCount; k++)
                        {
                            JavascriptString * str = SubString::New(input, trigramInfo->offsets[k], UnifiedRegex::TrigramInfo::PatternLength);
                            trigramInfo->cachedResult[k] = str;
                            arrayResult->DirectSetItemAt(k, str);
                        }
                        trigramInfo->hasCachedResultString = true;
                    }
                } // otherwise, there are no results and null will be returned

                if (updateHistory)
                {
                    PropagateLastMatch(scriptContext, /* isGlobal */ true, pattern->IsSticky(), regularExpression, input, lastSuccessfulMatch, lastActualMatch, true, true);
                }

                return lastSuccessfulMatch.IsUndefined() ? scriptContext->GetLibrary()->GetNull() : arrayResult;
            }
        }
#endif

        // If global regex, result array holds substrings for each match, and group bindings are ignored
        // If non-global regex, result array holds overall substring and each group binding substring

        const bool isGlobal = pattern->IsGlobal();
        const bool isSticky = pattern->IsSticky();
        // Hoisted out of the match loop (the walk advance reads it per match).
        const bool isUnicodePattern = RegexIsUnicodePattern(pattern);
        JavascriptArray* arrayResult = 0;
        RegexMatchState state;

        // If global = false and sticky = true, set offset = lastIndex, else set offset = 0
        CharCount offset = 0;
        if (!isGlobal && isSticky)
        {
            offset = regularExpression->GetLastIndex();
        }

        uint32 globalIndex = 0;
        PrimBeginMatch(state, scriptContext, pattern, inputStr, inputLength, false);

        do
        {
            if (offset > inputLength)
            {
                lastActualMatch.Reset();
                break;
            }
            lastActualMatch = PrimMatch(state, scriptContext, pattern, inputLength, offset);

            if (lastActualMatch.IsUndefined())
                break;
            lastSuccessfulMatch = lastActualMatch;
            if (!noResult)
            {
                if (arrayResult == 0)
                    arrayResult = CreateMatchResult(stackAllocationPointer, scriptContext, isGlobal, pattern->NumGroups(), input, pattern->IsHasIndices());
                JavascriptString *const matchedString = SubString::New(input, lastActualMatch.offset, lastActualMatch.length);
                if (isGlobal)
                    arrayResult->DirectSetItemAt(globalIndex, matchedString);
                else
                {
                    // The array's head segment up to length - 1 may not be filled. Write to the head segment element directly
                    // instead of calling a helper that expects the segment to be pre-filled.
                    Assert(globalIndex < arrayResult->GetHead()->length);
                    static_cast<SparseArraySegment<Var> *>(arrayResult->GetHead())->elements[globalIndex] = matchedString;
                }
                globalIndex++;
            }
            // Under u/v advance per code point (empty matches) and never
            // hand a pair-splitting end back to the matcher (see the walk
            // helpers above); non-u keeps the per-unit +1 advance.
            offset = isUnicodePattern
                ? RegexNextMatchStart(inputStr, inputLength, lastActualMatch.offset, lastActualMatch.EndOffset())
                : lastActualMatch.offset + max(lastActualMatch.length, static_cast<CharCountOrFlag>(1));
        } while (isGlobal);
        PrimEndMatch(state, scriptContext, pattern);
        if (updateHistory)
        {
            PropagateLastMatch(scriptContext, isGlobal, isSticky, regularExpression, input, lastSuccessfulMatch, lastActualMatch, true, true);
        }

        if (arrayResult == 0)
        {
            return scriptContext->GetLibrary()->GetNull();
        }

        const int numGroups = pattern->NumGroups();
        if (!isGlobal)
        {
            if (numGroups > 1)
            {
                // Overall match already captured in index 0 by above, so just grab the groups
                Var nonMatchValue = NonMatchValue(scriptContext, false);
                Field(Var) *elements = ((SparseArraySegment<Var>*)arrayResult->GetHead())->elements;
                for (uint groupId = 1; groupId < (uint)numGroups; groupId++)
                {
                    Assert(groupId < arrayResult->GetHead()->left + arrayResult->GetHead()->length);
                    elements[groupId] = GetGroup(scriptContext, pattern, input, nonMatchValue, groupId);
                }
            }
            JavascriptRegularExpressionResult::SetGroups(arrayResult, GetGroupsVar(scriptContext, pattern, input));
            if (pattern->IsHasIndices())
            {
                JavascriptRegularExpressionResult::SetIndices(arrayResult, CreateIndicesArray(scriptContext, pattern, input));
            }
            FinalizeMatchResult(scriptContext, /* isGlobal */ false, arrayResult, lastSuccessfulMatch);
        }
        else
        {
            FinalizeMatchResult(scriptContext, /* isGlobal */ true, arrayResult, lastSuccessfulMatch);
        }

        return arrayResult;
    }

    // RegExp.prototype.exec (ES5 15.10.6.2)
    Var RegexHelper::RegexExecImpl(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input, bool noResult, void *const stackAllocationPointer)
    {
        UnifiedRegex::RegexPattern* pattern = regularExpression->GetPattern();

#if ENABLE_REGEX_CONFIG_OPTIONS
        RegexHelperTrace(scriptContext, UnifiedRegex::RegexStats::Exec, regularExpression, input);
#endif

        const bool isGlobal = pattern->IsGlobal();
        const bool isSticky = pattern->IsSticky();
        CharCount offset;
        CharCount inputLength = input->GetLength();
        if (!GetInitialOffset(isGlobal, isSticky, regularExpression, inputLength, offset, scriptContext))
        {
            return scriptContext->GetLibrary()->GetNull();
        }

        UnifiedRegex::GroupInfo match; // initially undefined
        if (offset <= inputLength)
        {
            const char16* inputStr = input->GetString();
            match = SimpleMatch(scriptContext, pattern, inputStr, inputLength, offset);
        }

        // else: match remains undefined
        PropagateLastMatch(scriptContext, isGlobal, isSticky, regularExpression, input, match, RegexAdjustedLastMatchForPropagation(pattern, input->GetString(), inputLength, match), true, true);

        if (noResult || match.IsUndefined())
        {
            return scriptContext->GetLibrary()->GetNull();
        }

        const int numGroups = pattern->NumGroups();
        Assert(numGroups >= 0);
        const bool hasIndices = pattern->IsHasIndices();
        JavascriptArray* result = CreateExecResult(stackAllocationPointer, scriptContext, numGroups, input, match, hasIndices);
        Var nonMatchValue = NonMatchValue(scriptContext, false);
        Field(Var) *elements = ((SparseArraySegment<Var>*)result->GetHead())->elements;
        for (uint groupId = 0; groupId < (uint)numGroups; groupId++)
        {
            Assert(groupId < result->GetHead()->left + result->GetHead()->length);
            elements[groupId] = GetGroup(scriptContext, pattern, input, nonMatchValue, groupId);
        }
        JavascriptRegularExpressionResult::SetGroups(result, GetGroupsVar(scriptContext, pattern, input));
        if (hasIndices)
        {
            JavascriptRegularExpressionResult::SetIndices(result, CreateIndicesArray(scriptContext, pattern, input));
        }
        return result;
    }

    Var RegexHelper::RegexTest(ScriptContext* scriptContext, RecyclableObject* thisObj, JavascriptString *input)
    {
        if (scriptContext->GetConfig()->IsES6RegExSymbolsEnabled()
            && IsRegexTestObservable(thisObj, scriptContext))
        {
            return RegexEs6TestImpl(scriptContext, thisObj, input);
        }
        else
        {
            JavascriptRegExp* regularExpression =
                JavascriptRegExp::ToRegExp(thisObj, _u("RegExp.prototype.test"), scriptContext);
            return RegexEs5TestImpl(scriptContext, regularExpression, input);
        }
    }

    bool RegexHelper::IsRegexTestObservable(RecyclableObject* instance, ScriptContext* scriptContext)
    {
        DynamicObject* regexPrototype = scriptContext->GetLibrary()->GetRegExpPrototype();
        return !JavascriptRegExp::HasOriginalRegExType(instance)
            || JavascriptRegExp::HasObservableExec(regexPrototype);
    }

    Var RegexHelper::RegexEs6TestImpl(ScriptContext* scriptContext, RecyclableObject* thisObj, JavascriptString *input)
    {
        Var match = JavascriptRegExp::CallExec(thisObj, input, _u("RegExp.prototype.test"), scriptContext);
        return JavascriptBoolean::ToVar(!JavascriptOperators::IsNull(match), scriptContext);
    }

    // RegExp.prototype.test (ES5 15.10.6.3)
    Var RegexHelper::RegexEs5TestImpl(ScriptContext* scriptContext, JavascriptRegExp *regularExpression, JavascriptString *input)
    {
        UnifiedRegex::RegexPattern* pattern = regularExpression->GetPattern();
        const char16* inputStr = input->GetString();
        CharCount inputLength = input->GetLength();
        UnifiedRegex::GroupInfo match; // initially undefined

        const bool isGlobal = pattern->IsGlobal();
        const bool isSticky = pattern->IsSticky();
        const bool useCache = !isGlobal && !isSticky;

        UnifiedRegex::RegExpTestCache* cache = nullptr;
        JavascriptString * cachedInput = nullptr;
        uint cacheIndex = 0;
        bool cacheHit = false;
        bool cachedResult = false;
        if (useCache)
        {
            cache = pattern->EnsureTestCache();
            cacheIndex = UnifiedRegex::RegexPattern::GetTestCacheIndex(input);
            cachedInput = cache->inputArray[cacheIndex] != nullptr ? cache->inputArray[cacheIndex]->Get() : nullptr;
            cacheHit = cachedInput == input;
        }
#if ENABLE_REGEX_CONFIG_OPTIONS
        RegexHelperTrace(scriptContext, UnifiedRegex::RegexStats::Test, regularExpression, input);
        UnifiedRegex::RegexPattern::TraceTestCache(cacheHit, input, cachedInput, !useCache);
#endif

        if (cacheHit)
        {
            Assert(useCache);
            cachedResult = (cache->resultBV.Test(cacheIndex) != 0);

            // If our cache says this test should produce a match (which we aren't going to compute),
            // notify the Ctor to invalidate the last match so it must be recomputed before access.
            if (cachedResult)
            {
                InvalidateLastMatchOnCtor(scriptContext, regularExpression, input);
            }

            // for debug builds, let's still do the real test so we can validate values in the cache
#if !DBG
            return JavascriptBoolean::ToVar(cachedResult, scriptContext);
#endif
        }

        CharCount offset;
        if (!GetInitialOffset(isGlobal, isSticky, regularExpression, inputLength, offset, scriptContext))
        {
            if (useCache)
            {
                Assert(offset == 0);
                Assert(!cacheHit || cachedInput == input);
                Assert(!cacheHit || cachedResult == false);
                cache->inputArray[cacheIndex] = regularExpression->GetRecycler()->CreateWeakReferenceHandle(input);
                cache->resultBV.Clear(cacheIndex);
            }
            return scriptContext->GetLibrary()->GetFalse();
        }
        if (offset <= inputLength)
        {
            match = SimpleMatch(scriptContext, pattern, inputStr, inputLength, offset);
        }

        // else: match remains undefined
        PropagateLastMatch(scriptContext, isGlobal, isSticky, regularExpression, input, match, RegexAdjustedLastMatchForPropagation(pattern, inputStr, inputLength, match), true, true);
        bool wasFound = !match.IsUndefined();

        if (useCache)
        {
            Assert(offset == 0);
            Assert(!cacheHit || cachedInput == input);
            Assert(!cacheHit || cachedResult == wasFound);
            cache->inputArray[cacheIndex] = regularExpression->GetRecycler()->CreateWeakReferenceHandle(input);
            if (wasFound)
            {
                cache->resultBV.Set(cacheIndex);
            }
            else
            {
                cache->resultBV.Clear(cacheIndex);
            }
        }
        return JavascriptBoolean::ToVar(wasFound, scriptContext);
    }

    template<typename GroupFn, typename NamedGroupFn>
    void RegexHelper::ReplaceFormatString
        ( ScriptContext* scriptContext
        , int numGroups
        , GroupFn getGroup
        , bool namedSubstitutionEnabled
        , NamedGroupFn getNamedGroup
        , JavascriptString* input
        , const char16* matchedString
        , UnifiedRegex::GroupInfo match
        , JavascriptString* replace
        , int substitutions
        , __in_ecount(substitutions) CharCount* substitutionOffsets
        , CompoundString::Builder<64 * sizeof(void *) / sizeof(char16)>& concatenated )
    {
        Var nonMatchValue = NonMatchValue(scriptContext, false);
        const CharCount inputLength = input->GetLength();
        const char16* replaceStr = replace->GetString();
        const CharCount replaceLength = replace->GetLength();

        CharCount offset = 0;
        for (int i = 0; i < substitutions; i++)
        {
            CharCount substitutionOffset = substitutionOffsets[i];
            concatenated.Append(replace, offset, substitutionOffset - offset);
            char16 currentChar = replaceStr[substitutionOffset + 1];
            if (currentChar >= _u('0') && currentChar <= _u('9'))
            {
                // We've found a substitution ref, like $32.  In accordance with the standard (sec-getsubstitution),
                // we recognize at most two decimal digits after the dollar sign.
                uint16 captureIndex = (uint16)(currentChar - _u('0'));
                Assert(captureIndex < 10); // numeric value of single decimal digit

                offset = substitutionOffset + 2;

                if (offset < replaceLength)
                {
                    currentChar = replaceStr[substitutionOffset + 2];
                    if (currentChar >= _u('0') && currentChar <= _u('9'))
                    {
                        uint16 tempCaptureIndex = (10 * captureIndex) + (uint16)(currentChar - _u('0'));
                        Assert(tempCaptureIndex < 100); // numeric value of 2-digit positive decimal number
                        if (tempCaptureIndex < numGroups)
                        {
                            captureIndex = tempCaptureIndex;
                            offset = substitutionOffset + 3;
                        }
                    }
                }

                Assert(captureIndex < 100); // as above, value of 2-digit positive decimal number
                if (captureIndex < numGroups && (captureIndex != 0))
                {
                    Var group = getGroup(captureIndex, nonMatchValue);
                    if (VarIs<JavascriptString>(group))
                        concatenated.Append(UnsafeVarTo<JavascriptString>(group));
                    else if (group != nonMatchValue)
                        concatenated.Append(replace, substitutionOffset, offset - substitutionOffset);
                }
                else
                    concatenated.Append(replace, substitutionOffset, offset - substitutionOffset);
            }
            else
            {
                switch (currentChar)
                {
                case _u('$'): // literal '$' character
                    concatenated.Append(_u('$'));
                    offset = substitutionOffset + 2;
                    break;
                case _u('&'): // matched string
                    concatenated.Append(matchedString, match.length);
                    offset = substitutionOffset + 2;
                    break;
                case _u('`'): // left context
                    concatenated.Append(input, 0, match.offset);
                    offset = substitutionOffset + 2;
                    break;
                case _u('\''): // right context
                    if (match.EndOffset() < inputLength)
                    {
                        concatenated.Append(input, match.EndOffset(), inputLength - match.EndOffset());
                    }
                    offset = substitutionOffset + 2;
                    break;
                case _u('<'): // named capture group substitution $<name>
                    if (namedSubstitutionEnabled)
                    {
                        // GetSubstitution: everything through the first '>' is
                        // consumed as the group name; an unknown or unmatched
                        // name contributes nothing. Without a closing '>' the
                        // '$' stays literal. When named substitution is not
                        // enabled (no named captures / no groups object), the
                        // '$' stays literal as well (default arm).
                        CharCount gt = substitutionOffset + 2;
                        while (gt < replaceLength && replaceStr[gt] != _u('>'))
                        {
                            gt++;
                        }
                        if (gt >= replaceLength)
                        {
                            concatenated.Append(_u('$'));
                            offset = substitutionOffset + 1;
                        }
                        else
                        {
                            Var namedCapture = getNamedGroup(
                                replaceStr + substitutionOffset + 2,
                                gt - (substitutionOffset + 2));
                            if (!JavascriptOperators::IsUndefined(namedCapture))
                            {
                                JavascriptString* namedCaptureString =
                                    JavascriptConversion::ToString(namedCapture, scriptContext);
                                concatenated.Append(namedCaptureString);
                            }
                            offset = gt + 1;
                            // The name may contain '$' characters that the
                            // substitution pre-scan registered as offsets;
                            // they are part of this consumed name.
                            while (i + 1 < substitutions && substitutionOffsets[i + 1] < offset)
                            {
                                i++;
                            }
                        }
                        break;
                    }
                    // fall-through: literal '$'
                default:
                    concatenated.Append(_u('$'));
                    offset = substitutionOffset + 1;
                    break;
                }
            }
        }
        concatenated.Append(replace, offset, replaceLength - offset);
    }

    int RegexHelper::GetReplaceSubstitutions(const char16 * const replaceStr, CharCount const replaceLength,
        ArenaAllocator * const tempAllocator, CharCount** const substitutionOffsetsOut)
    {
        int substitutions = 0;

        for (CharCount i = 0; i < replaceLength; i++)
        {
            if (replaceStr[i] == _u('$'))
            {
                if (++i < replaceLength)
                {
                    substitutions++;
                }
            }
        }

        if (substitutions > 0)
        {
            CharCount* substitutionOffsets = AnewArray(tempAllocator, CharCount, substitutions);
            substitutions = 0;
            for (CharCount i = 0; i < replaceLength; i++)
            {
                if (replaceStr[i] == _u('$'))
                {
                    if (i < (replaceLength - 1))
                    {
#pragma prefast(suppress:26000, "index doesn't overflow the buffer")
                        substitutionOffsets[substitutions] = i;
                        i++;
                        substitutions++;
                    }
                }
            }
            *substitutionOffsetsOut = substitutionOffsets;
        }

        return substitutions;
    }

    Var RegexHelper::RegexReplaceImpl(ScriptContext* scriptContext, RecyclableObject* thisObj, JavascriptString* input, JavascriptString* replace, bool noResult)
    {
        ScriptConfiguration const * scriptConfig = scriptContext->GetConfig();

        if (scriptConfig->IsES6RegExSymbolsEnabled() && IsRegexSymbolReplaceObservable(thisObj, scriptContext))
        {
            return RegexEs6ReplaceImpl(scriptContext, thisObj, input, replace, noResult);
        }
        else
        {
            PCWSTR varName = scriptConfig->IsES6RegExSymbolsEnabled()
                ? _u("RegExp.prototype[Symbol.replace]")
                : _u("String.prototype.replace");
            JavascriptRegExp* regularExpression = JavascriptRegExp::ToRegExp(thisObj, varName, scriptContext);
            return RegexEs5ReplaceImpl(scriptContext, regularExpression, input, replace, noResult);
        }
    }

    bool RegexHelper::IsRegexSymbolReplaceObservable(RecyclableObject* instance, ScriptContext* scriptContext)
    {
        DynamicObject* regexPrototype = scriptContext->GetLibrary()->GetRegExpPrototype();
        return !JavascriptRegExp::HasOriginalRegExType(instance)
            || JavascriptRegExp::HasObservableUnicodeFlag(regexPrototype)
            || JavascriptRegExp::HasObservableExec(regexPrototype)
            || JavascriptRegExp::HasObservableGlobalFlag(regexPrototype);
    }

    Var RegexHelper::RegexEs6ReplaceImpl(ScriptContext* scriptContext, RecyclableObject* thisObj, JavascriptString* input, JavascriptString* replace, bool noResult)
    {
        auto appendReplacement = [&](
            CompoundString::Builder<64 * sizeof(void *) / sizeof(char16)>& resultBuilder,
            ArenaAllocator* tempAlloc,
            JavascriptString* matchStr,
            int numberOfCaptures,
            Field(Var)* captures,
            CharCount position,
            Var namedCaptures)
        {
            CharCount* substitutionOffsets = nullptr;
            int substitutions = GetReplaceSubstitutions(
                replace->GetString(),
                replace->GetLength(),
                tempAlloc,
                &substitutionOffsets);
            auto getGroup = [&](int captureIndex, Var nonMatchValue) {
                return captureIndex <= numberOfCaptures ? PointerValue(captures[captureIndex]) : nonMatchValue;
            };
            // GetSubstitution receives the coerced "groups" value: when
            // undefined (pattern without named captures, or an exec result
            // without a groups property), $<name> stays literal.
            const bool namedSubstitutionEnabled = !JavascriptOperators::IsUndefined(namedCaptures);
            RecyclableObject* namedCapturesObject = namedSubstitutionEnabled
                ? VarTo<RecyclableObject>(namedCaptures)
                : nullptr;
            auto getNamedGroup = [=](const char16* name, CharCount nameLen) -> Var {
                PropertyId propertyId = scriptContext->GetOrAddPropertyIdTracked(name, static_cast<int>(nameLen));
                return JavascriptOperators::GetProperty(namedCapturesObject, propertyId, scriptContext);
            };
            UnifiedRegex::GroupInfo match(position, matchStr->GetLength());
            int numGroups = numberOfCaptures + 1; // Take group 0 into account.
            ReplaceFormatString(
                scriptContext,
                numGroups,
                getGroup,
                namedSubstitutionEnabled,
                getNamedGroup,
                input,
                matchStr->GetString(),
                match,
                replace,
                substitutions,
                substitutionOffsets,
                resultBuilder);
        };
        return RegexEs6ReplaceImpl(scriptContext, thisObj, input, appendReplacement, noResult);
    }

    Var RegexHelper::RegexEs6ReplaceImpl(ScriptContext* scriptContext, RecyclableObject* thisObj, JavascriptString* input, RecyclableObject* replaceFn)
    {
        auto appendReplacement = [&](
            CompoundString::Builder<64 * sizeof(void *) / sizeof(char16)>& resultBuilder,
            ArenaAllocator* tempAlloc,
            JavascriptString* matchStr,
            int numberOfCaptures,
            Field(Var)* captures,
            CharCount position,
            Var namedCaptures)
        {
            // replaceFn Arguments:
            //
            // 0: this
            // 1: matched
            // 2: capture1
            // ...
            // N + 1: capture N
            // N + 2: position
            // N + 3: input
            // N + 4: namedCaptures (only when not undefined, per @@replace)

            // Number of captures can be at most 99, so we won't overflow.
            ushort argCount = (ushort) numberOfCaptures + 4;

            PROBE_STACK_NO_DISPOSE(scriptContext, (argCount + 1) * sizeof(Var));

            ThreadContext* threadContext = scriptContext->GetThreadContext();

            Var* args = (Var*) _alloca((argCount + 1) * sizeof(Var));

            args[0] = scriptContext->GetLibrary()->GetUndefined();
#pragma prefast(suppress:6386, "The write is within the bounds")
            args[1] = matchStr;
            for (int i = 1; i <= numberOfCaptures; ++i)
            {
                args[i + 1] = captures[i];
            }
            args[numberOfCaptures + 2] = JavascriptNumber::ToVar(position, scriptContext);
            args[numberOfCaptures + 3] = input;
            if (!JavascriptOperators::IsUndefined(namedCaptures))
            {
                args[argCount] = namedCaptures;
                argCount++;
            }
            Js::Var replaceFnResult = threadContext->ExecuteImplicitCall(replaceFn, Js::ImplicitCall_Accessor, [=]()->Js::Var
            {
                return JavascriptFunction::CallFunction<true>(replaceFn, replaceFn->GetEntryPoint(), Arguments(CallInfo(argCount), args));
            });
            JavascriptString* replace = JavascriptConversion::ToString(replaceFnResult, scriptContext);

            resultBuilder.Append(replace);
        };
        return RegexEs6ReplaceImpl(scriptContext, thisObj, input, appendReplacement, /* noResult */ false);
    }

    template<typename ReplacementFn>
    Var RegexHelper::RegexEs6ReplaceImpl(ScriptContext* scriptContext, RecyclableObject* thisObj, JavascriptString* input, ReplacementFn appendReplacement, bool noResult)
    {
        // ES2026 @@replace: global and fullUnicode ('u' OR 'v') derive from
        // ONE observable Get(R, "flags") + ToString (substrate fallback for
        // a genuine RegExp whose "flags" accessor was not installed — the
        // same fallback @@matchAll makes; a user value is never ignored).
        bool global = false;
        bool unicode = false; // fullUnicode; only read by the loop when global.
        JavascriptRegExp::ParseBuiltinFlags(
            JavascriptRegExp::GetFlagsStringForBuiltin(thisObj, scriptContext), global, unicode);
        if (global)
        {
            JavascriptRegExp::SetLastIndexProperty(thisObj, TaggedInt::ToVarUnchecked(0), scriptContext);
        }

        JavascriptString* accumulatedResult = nullptr;

        Recycler* recycler = scriptContext->GetRecycler();

        JsUtil::List<RecyclableObject*>* results = RecyclerNew(recycler, JsUtil::List<RecyclableObject*>, recycler);

        while (true)
        {
            PCWSTR varName = _u("RegExp.prototype[Symbol.replace]");
            Var result = JavascriptRegExp::CallExec(thisObj, input, varName, scriptContext);
            if (JavascriptOperators::IsNull(result))
            {
                break;
            }

            RecyclableObject* resultObj = ExecResultToRecyclableObject(result);

            results->Add(resultObj);

            if (!global)
            {
                break;
            }

            JavascriptString* matchStr = GetMatchStrFromResult(resultObj, scriptContext);
            AdvanceLastIndex(thisObj, input, matchStr, unicode, scriptContext);
        }

        CompoundString::Builder<64 * sizeof(void *) / sizeof(char16)> accumulatedResultBuilder(scriptContext);
        CharCount inputLength = input->GetLength();
        CharCount nextSourcePosition = 0;

        size_t previousNumberOfCapturesToKeep = 0;
        Field(Var)* captures = nullptr;
        // ES6RegExSymbols gates RegExp.prototype[Symbol.replace] and every
        // caller of this impl (the flag-off replace paths, including
        // replaceAll's direct RegexReplace calls, run RegexEs5ReplaceImpl).
        // Guard the tracked-id lookup so no path can pay it with the flag off.
        const PropertyId groupsPropertyId = scriptContext->GetConfig()->IsES6RegExSymbolsEnabled()
            ? scriptContext->GetOrAddPropertyIdTracked(_u("groups"))
            : Constants::NoProperty;

        BEGIN_TEMP_ALLOCATOR(tempAlloc, scriptContext, _u("RegexHelper"))
        {
            results->Map([&](int resultIndex, RecyclableObject* resultObj) {
                int64 length = JavascriptConversion::ToLength(
                    JavascriptOperators::GetProperty(resultObj, PropertyIds::length, scriptContext),
                    scriptContext);
                uint64 numberOfCaptures = (uint64) max(length - 1, (int64) 0);

                JavascriptString* matchStr = GetMatchStrFromResult(resultObj, scriptContext);

                int64 index = JavascriptConversion::ToLength(
                    JavascriptOperators::GetProperty(resultObj, PropertyIds::index, scriptContext),
                    scriptContext);
                CharCount position = max(
                    min(JavascriptRegExp::GetIndexOrMax(index), inputLength),
                    (CharCount) 0);

                // Capture groups can be referenced using at most two digits.
                const uint64 maxNumberOfCaptures = 99;
                size_t numberOfCapturesToKeep = (size_t) min(numberOfCaptures, maxNumberOfCaptures);
                if (captures == nullptr)
                {
                    captures = RecyclerNewArray(recycler, Field(Var), numberOfCapturesToKeep + 1);
                }
                else if (numberOfCapturesToKeep != previousNumberOfCapturesToKeep)
                {
                    size_t existingBytes = (previousNumberOfCapturesToKeep + 1) * sizeof(Var*);
                    size_t requestedBytes = (numberOfCapturesToKeep + 1) * sizeof(Var*);
                    captures = (Field(Var)*) recycler->Realloc(captures, existingBytes, requestedBytes);
                }
                previousNumberOfCapturesToKeep = numberOfCapturesToKeep;

                for (uint64 i = 1; i <= numberOfCaptures; ++i)
                {
                    Var nextCapture = JavascriptOperators::GetItem(resultObj, i, scriptContext);
                    if (!JavascriptOperators::IsUndefined(nextCapture))
                    {
                        nextCapture = JavascriptConversion::ToString(nextCapture, scriptContext);
                    }

                    if (i <= numberOfCapturesToKeep)
                    {
                        captures[i] = nextCapture;
                    }
                }

                // ES2021 @@replace: "groups" is read after the captures and,
                // when not undefined, coerced through ToObject (a null
                // "groups" throws TypeError). The coerced value feeds the
                // $<name> substitutions and, when not undefined, is passed to
                // a replacement function as the final argument.
                Var namedCaptures = scriptContext->GetLibrary()->GetUndefined();
                if (groupsPropertyId != Constants::NoProperty)
                {
                    Var namedCapturesValue = JavascriptOperators::GetProperty(resultObj, groupsPropertyId, scriptContext);
                    if (!JavascriptOperators::IsUndefined(namedCapturesValue))
                    {
                        RecyclableObject* namedCapturesObject = nullptr;
                        if (!JavascriptConversion::ToObject(namedCapturesValue, scriptContext, &namedCapturesObject))
                        {
                            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedObject, _u("RegExp.prototype[Symbol.replace]"));
                        }
                        namedCaptures = namedCapturesObject;
                    }
                }

                if (position >= nextSourcePosition)
                {
                    CharCount substringLength = position - nextSourcePosition;
                    accumulatedResultBuilder.Append(input, nextSourcePosition, substringLength);

                    appendReplacement(accumulatedResultBuilder, tempAlloc, matchStr, (int) numberOfCapturesToKeep, captures, position, namedCaptures);

                    nextSourcePosition = JavascriptRegExp::AddIndex(position, matchStr->GetLength());
                }
            });
        }
        END_TEMP_ALLOCATOR(tempAlloc, scriptContext);

        if (nextSourcePosition < inputLength)
        {
            CharCount substringLength = inputLength - nextSourcePosition;
            accumulatedResultBuilder.Append(input, nextSourcePosition, substringLength);
        }

        accumulatedResult =  accumulatedResultBuilder.ToString();

        Assert(accumulatedResult != nullptr);
        return accumulatedResult;
    }

    // String.prototype.replace, replace value has been converted to a string (ES5 15.5.4.11)
    Var RegexHelper::RegexEs5ReplaceImpl(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input, JavascriptString* replace, bool noResult)
    {
        UnifiedRegex::RegexPattern* pattern = regularExpression->GetPattern();
        const char16* replaceStr = replace->GetString();
        CharCount replaceLength = replace->GetLength();
        const char16* inputStr = input->GetString();
        CharCount inputLength = input->GetLength();

        JavascriptString* newString = nullptr;

#if ENABLE_REGEX_CONFIG_OPTIONS
        RegexHelperTrace(scriptContext, UnifiedRegex::RegexStats::Replace, regularExpression, input, replace);
#endif

        RegexMatchState state;
        PrimBeginMatch(state, scriptContext, pattern, inputStr, inputLength, true);

        UnifiedRegex::GroupInfo lastActualMatch;
        UnifiedRegex::GroupInfo lastSuccessfulMatch;
        const bool isGlobal = pattern->IsGlobal();
        const bool isSticky = pattern->IsSticky();
        // Hoisted out of the replace loop (the walk advance reads it per match).
        const bool isUnicodePattern = RegexIsUnicodePattern(pattern);
        CharCount offset = 0;
        if (!isGlobal && isSticky)
        {
            offset = regularExpression->GetLastIndex();
        }

        if (!noResult)
        {
            CharCount* substitutionOffsets = nullptr;
            int substitutions = GetReplaceSubstitutions(replaceStr, replaceLength,
                 state.tempAllocatorObj->GetAllocator(), &substitutionOffsets);

            // Use to see if we already have partial result populated in concatenated
            CompoundString::Builder<64 * sizeof(void *) / sizeof(char16)> concatenated(scriptContext);

            // If lastIndex > 0, append input[0..offset] characters to the result
            if (offset > 0)
            {
                concatenated.Append(input, 0, min(offset, inputLength));
            }

            do
            {
                if (offset > inputLength)
                {
                    lastActualMatch.Reset();
                    break;
                }

                lastActualMatch = PrimMatch(state, scriptContext, pattern, inputLength, offset);

                if (lastActualMatch.IsUndefined())
                    break;

                lastSuccessfulMatch = lastActualMatch;
                concatenated.Append(input, offset, lastActualMatch.offset - offset);
                if (substitutionOffsets != 0)
                {
                    auto getGroup = [&](int captureIndex, Var nonMatchValue) {
                        return GetGroup(scriptContext, pattern, input, nonMatchValue, captureIndex);
                    };
                    // $<name> resolves against the pattern's static name
                    // registry and the live match (the ES5 fast path never
                    // materializes a groups object here); enabled only when
                    // the pattern has named captures, matching GetSubstitution
                    // with namedCaptures undefined otherwise.
                    UnifiedRegex::Program* program = pattern->rep.unified.program;
                    const bool namedSubstitutionEnabled = program->HasNamedGroups();
                    Var undefined = scriptContext->GetLibrary()->GetUndefined();
                    auto getNamedGroup = [=](const char16* name, CharCount nameLen) -> Var {
                        // ES2025 duplicate named groups: the substitution
                        // uses the value of the participating group with
                        // this name (last participant; at most one is live).
                        int groupId = ResolveNamedGroupId(program, pattern, name, nameLen);
                        if (groupId < 0)
                        {
                            return undefined;
                        }
                        return GetString(scriptContext, input, undefined, pattern->GetGroup(groupId));
                    };
                    const char16* matchedString = inputStr + lastActualMatch.offset;
                    ReplaceFormatString(scriptContext, pattern->NumGroups(), getGroup, namedSubstitutionEnabled, getNamedGroup, input, matchedString, lastActualMatch, replace, substitutions, substitutionOffsets, concatenated);
                }
                else
                {
                    concatenated.Append(replace);
                }
                if (lastActualMatch.length == 0)
                {
                    // Under u/v the empty-match advance steps a whole code
                    // point; stepping one unit into a surrogate pair would
                    // hand the next attempt a pair-splitting start, which
                    // Matcher::Match snaps back to this same empty match.
                    // The stepped code point is appended verbatim. Non-u
                    // keeps the +1 unit advance.
                    const CharCount step = isUnicodePattern
                        ? RegexAdvanceStringIndexUnicode(inputStr, inputLength, lastActualMatch.offset) - lastActualMatch.offset
                        : 1;
                    if (lastActualMatch.offset < inputLength)
                    {
                        concatenated.Append(inputStr[lastActualMatch.offset]);
                        if (step == 2)
                        {
                            concatenated.Append(inputStr[lastActualMatch.offset + 1]);
                        }
                    }
                    offset = lastActualMatch.offset + step;
                }
                else
                {
                    offset = isUnicodePattern
                        ? RegexNextMatchStart(inputStr, inputLength, lastActualMatch.offset, lastActualMatch.EndOffset())
                        : lastActualMatch.EndOffset();
                }
            }
            while (isGlobal);

            if (offset == 0)
            {
                // There was no successful match so the result is the input string.
                newString = input;
            }
            else
            {
                if (offset < inputLength)
                {
                    concatenated.Append(input, offset, inputLength - offset);
                }
                newString = concatenated.ToString();
            }
            substitutionOffsets = 0;
        }
        else
        {
            do
            {
                if (offset > inputLength)
                {
                    lastActualMatch.Reset();
                    break;
                }
                lastActualMatch = PrimMatch(state, scriptContext, pattern, inputLength, offset);

                if (lastActualMatch.IsUndefined())
                    break;

                lastSuccessfulMatch = lastActualMatch;
                // See the empty-match advance above: u/v walks step whole
                // code points and never re-enter a pair-splitting end.
                offset = !isUnicodePattern
                    ? (lastActualMatch.length == 0 ? lastActualMatch.offset + 1 : lastActualMatch.EndOffset())
                    : RegexNextMatchStart(inputStr, inputLength, lastActualMatch.offset, lastActualMatch.EndOffset());
            }
            while (isGlobal);
            newString = scriptContext->GetLibrary()->GetEmptyString();
        }

        PrimEndMatch(state, scriptContext, pattern);
        PropagateLastMatch(scriptContext, isGlobal, isSticky, regularExpression, input, lastSuccessfulMatch, lastActualMatch, true, true);
        return newString;
    }

    Var RegexHelper::RegexReplaceImpl(ScriptContext* scriptContext, RecyclableObject* thisObj, JavascriptString* input, RecyclableObject* replacefn)
    {
        ScriptConfiguration const * scriptConfig = scriptContext->GetConfig();

        if (scriptConfig->IsES6RegExSymbolsEnabled() && IsRegexSymbolReplaceObservable(thisObj, scriptContext))
        {
            return RegexEs6ReplaceImpl(scriptContext, thisObj, input, replacefn);
        }
        else
        {
            PCWSTR varName = scriptConfig->IsES6RegExSymbolsEnabled()
                ? _u("RegExp.prototype[Symbol.replace]")
                : _u("String.prototype.replace");
            JavascriptRegExp* regularExpression = JavascriptRegExp::ToRegExp(thisObj, varName, scriptContext);
            return RegexEs5ReplaceImpl(scriptContext, regularExpression, input, replacefn);
        }
    }

    // String.prototype.replace, replace value is a function (ES5 15.5.4.11)
    Var RegexHelper::RegexEs5ReplaceImpl(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input, RecyclableObject* replacefn)
    {
        UnifiedRegex::RegexPattern* pattern = regularExpression->GetPattern();
        JavascriptString* newString = nullptr;
        const char16* inputStr = input->GetString();
        CharCount inputLength = input->GetLength();
        const uint16 numGroups = pattern->NumGroups();
        Var nonMatchValue = NonMatchValue(scriptContext, false);
        UnifiedRegex::GroupInfo lastMatch; // initially undefined

        // Regex parser should ensure this condition holds, but let's be doubly sure.
        // numGroups is always positive because the entire regex counts as a capturing group.
        AssertOrFailFast(0 < numGroups && numGroups <= INT16_MAX);

#if ENABLE_REGEX_CONFIG_OPTIONS
        RegexHelperTrace(scriptContext, UnifiedRegex::RegexStats::Replace, regularExpression, input, scriptContext->GetLibrary()->CreateStringFromCppLiteral(_u("<replace function>")));
#endif

        RegexMatchState state;
        PrimBeginMatch(state, scriptContext, pattern, inputStr, inputLength, false);

        // NOTE: These must be kept out of the scope of the try below!
        const bool isGlobal = pattern->IsGlobal();
        const bool isSticky = pattern->IsSticky();
        // Hoisted out of the replace loop (the walk advance reads it per match).
        const bool isUnicodePattern = RegexIsUnicodePattern(pattern);

        // If global = true, set lastIndex to 0 in case it is used in replacefn
        if (isGlobal)
        {
            regularExpression->SetLastIndex(0);
        }

        // If global = false and sticky = true, set offset = lastIndex, else set offset = 0
        CharCount offset = 0;
        if (!isGlobal && isSticky)
        {
            offset = regularExpression->GetLastIndex();
        }

        CompoundString::Builder<64 * sizeof(void *) / sizeof(char16)> concatenated(scriptContext);
        UnifiedRegex::GroupInfo lastActualMatch;
        UnifiedRegex::GroupInfo lastSuccessfulMatch;

        // Replace function must be called with arguments (<function's this>, group0, ..., groupn, offset, input, [groups])
        // The garbage collector must know about this array since it is being passed back into script land
        Var* replaceArgs;
        // ES2018: when the pattern has named captures, the final argument is
        // the match's groups object.
        const bool hasNamedGroups = pattern->rep.unified.program->HasNamedGroups();
        const uint16 argCount = UInt16Math::Add(numGroups, hasNamedGroups ? 4u : 3u);
        PROBE_STACK_NO_DISPOSE(scriptContext, argCount * sizeof(Var));
        replaceArgs = (Var*)_alloca(argCount * sizeof(Var));
        replaceArgs[0] = scriptContext->GetLibrary()->GetUndefined();
        replaceArgs[numGroups + 2] = input;

        if (offset > 0)
        {
            concatenated.Append(input, 0, min(offset, inputLength));
        }

        do
        {
            if (offset > inputLength)
            {
                lastActualMatch.Reset();
                break;
            }

            lastActualMatch = PrimMatch(state, scriptContext, pattern, inputLength, offset);

            if (lastActualMatch.IsUndefined())
                break;

            lastSuccessfulMatch = lastActualMatch;
            for (int groupId = 0;  groupId < numGroups; groupId++)
                replaceArgs[groupId + 1] = GetGroup(scriptContext, pattern, input, nonMatchValue, groupId);
            replaceArgs[numGroups + 1] = JavascriptNumber::ToVar(lastActualMatch.offset, scriptContext);
            if (hasNamedGroups)
            {
                replaceArgs[numGroups + 3] = CreateGroupsObject(scriptContext, pattern, input);
            }

            // The called function must see the global state updated by the current match
            // (Should the function reach into a RegExp field, the pattern will still be valid, thus there's no
            //  danger of the primitive regex matcher being re-entered)

            // WARNING: We go off into script land here, which way in turn invoke a regex operation, even on the
            //          same regex.

            ThreadContext* threadContext = scriptContext->GetThreadContext();
            Var replaceVar = threadContext->ExecuteImplicitCall(replacefn, ImplicitCall_Accessor, [=]()->Js::Var
            {
                    return JavascriptFunction::CallFunction<true>(replacefn, replacefn->GetEntryPoint(), Arguments(CallInfo(argCount), replaceArgs));
            });
            JavascriptString* replace = JavascriptConversion::ToString(replaceVar, scriptContext);
            concatenated.Append(input, offset, lastActualMatch.offset - offset);
            concatenated.Append(replace);

            if (lastActualMatch.length == 0)
            {
                // See the string-replace empty-match advance: under u/v step
                // a whole code point (appending it verbatim) so the next
                // attempt cannot start on a pair's second half.
                const CharCount step = isUnicodePattern
                    ? RegexAdvanceStringIndexUnicode(inputStr, inputLength, lastActualMatch.offset) - lastActualMatch.offset
                    : 1;
                if (lastActualMatch.offset < inputLength)
                {
                    concatenated.Append(inputStr[lastActualMatch.offset]);
                    if (step == 2)
                    {
                        concatenated.Append(inputStr[lastActualMatch.offset + 1]);
                    }
                }
                offset = lastActualMatch.offset + step;
            }
            else
            {
                offset = isUnicodePattern
                    ? RegexNextMatchStart(inputStr, inputLength, lastActualMatch.offset, lastActualMatch.EndOffset())
                    : lastActualMatch.EndOffset();
            }
        }
        while (isGlobal);

        PrimEndMatch(state, scriptContext, pattern);

        if (offset == 0)
        {
            // There was no successful match so the result is the input string.
            newString = input;
        }
        else
        {
            if (offset < inputLength)
            {
                concatenated.Append(input, offset, inputLength - offset);
            }
            newString = concatenated.ToString();
        }

        PropagateLastMatch(scriptContext, isGlobal, isSticky, regularExpression, input, lastSuccessfulMatch, lastActualMatch, true, true);
        return newString;
    }

    Var RegexHelper::StringReplace(JavascriptString* match, JavascriptString* input, JavascriptString* replace)
    {
        CharCount matchedIndex = JavascriptString::strstr(input, match, true);
        if (matchedIndex == CharCountFlag)
        {
            return input;
        }

        const char16 *const replaceStr = replace->GetString();

        // Unfortunately, due to the possibility of there being $ escapes, we can't just wmemcpy the replace string. Check if we
        // have a small replace string that we can quickly scan for '$', to see if we can just wmemcpy.
        bool definitelyNoEscapes = replace->GetLength() == 0;
        if(!definitelyNoEscapes && replace->GetLength() <= 8)
        {
            CharCount i = 0;
            for(; i < replace->GetLength() && replaceStr[i] != _u('$'); ++i);
            definitelyNoEscapes = i >= replace->GetLength();
        }

        if(definitelyNoEscapes)
        {
            const char16* inputStr = input->GetString();
            const char16* prefixStr = inputStr;
            CharCount prefixLength = (CharCount)matchedIndex;
            const char16* postfixStr = inputStr + prefixLength + match->GetLength();
            CharCount postfixLength = input->GetLength() - prefixLength - match->GetLength();
            CharCount newLength = prefixLength + postfixLength + replace->GetLength();
            BufferStringBuilder bufferString(newLength, match->GetScriptContext());
            bufferString.SetContent(prefixStr, prefixLength,
                                    replaceStr, replace->GetLength(),
                                    postfixStr, postfixLength);
            return bufferString.ToString();
        }

        CompoundString::Builder<64 * sizeof(void *) / sizeof(char16)> concatenated(input->GetScriptContext());

        // Copy portion of input string that precedes the matched substring
        concatenated.Append(input, 0, matchedIndex);

        // Copy the replace string with substitutions
        CharCount i = 0, j = 0;
        for(; j < replace->GetLength(); ++j)
        {
            if(replaceStr[j] == _u('$') && j + 1 < replace->GetLength())
            {
                switch(replaceStr[j + 1])
                {
                    case _u('$'): // literal '$'
                        ++j;
                        concatenated.Append(replace, i, j - i);
                        i = j + 1;
                        break;

                    case _u('&'): // matched substring
                        concatenated.Append(replace, i, j - i);
                        concatenated.Append(match);
                        ++j;
                        i = j + 1;
                        break;

                    case _u('`'): // portion of input string that precedes the matched substring
                        concatenated.Append(replace, i, j - i);
                        concatenated.Append(input, 0, matchedIndex);
                        ++j;
                        i = j + 1;
                        break;

                    case _u('\''): // portion of input string that follows the matched substring
                        concatenated.Append(replace, i, j - i);
                        concatenated.Append(
                            input,
                            matchedIndex + match->GetLength(),
                            input->GetLength() - matchedIndex - match->GetLength());
                        ++j;
                        i = j + 1;
                        break;

                    default: // take both the initial '$' and the following character literally
                        ++j;
                }
            }
        }
        Assert(i <= j);
        concatenated.Append(replace, i, j - i);

        // Copy portion of input string that follows the matched substring
        concatenated.Append(input, matchedIndex + match->GetLength(), input->GetLength() - matchedIndex - match->GetLength());

        return concatenated.ToString();
    }

    Var RegexHelper::StringReplace(ScriptContext* scriptContext, JavascriptString* match, JavascriptString* input, RecyclableObject* replacefn)
    {
        CharCount indexMatched = JavascriptString::strstr(input, match, true);
        Assert(match->GetScriptContext() == scriptContext);
        Assert(input->GetScriptContext() == scriptContext);

        if (indexMatched != CharCountFlag)
        {
            ThreadContext* threadContext = scriptContext->GetThreadContext();
            Var replaceVar = threadContext->ExecuteImplicitCall(replacefn, ImplicitCall_Accessor, [=]()->Js::Var
            {
                Var pThis = scriptContext->GetLibrary()->GetUndefined();
                return CALL_FUNCTION(threadContext, replacefn, CallInfo(4), pThis, match, JavascriptNumber::ToVar((int)indexMatched, scriptContext), input);
            });
            JavascriptString* replace = JavascriptConversion::ToString(replaceVar, scriptContext);
            const char16* inputStr = input->GetString();
            const char16* prefixStr = inputStr;
            CharCount prefixLength = indexMatched;
            const char16* postfixStr = inputStr + prefixLength + match->GetLength();
            CharCount postfixLength = input->GetLength() - prefixLength - match->GetLength();
            CharCount newLength = prefixLength + postfixLength + replace->GetLength();
            BufferStringBuilder bufferString(newLength, match->GetScriptContext());
            bufferString.SetContent(prefixStr, prefixLength,
                replace->GetString(), replace->GetLength(),
                postfixStr, postfixLength);
            return bufferString.ToString();
        }

        return input;
    }

    void RegexHelper::AppendSubString(ScriptContext* scriptContext, JavascriptArray* ary, JavascriptString* input, CharCount startInclusive, CharCount endExclusive)
    {
        Assert(endExclusive >= startInclusive);
        Assert(endExclusive <= input->GetLength());
        CharCount length = endExclusive - startInclusive;
        JavascriptString* subString;
        if (length == 0)
        {
            subString = scriptContext->GetLibrary()->GetEmptyString();
        }
        else if (length == 1)
        {
            subString = scriptContext->GetLibrary()->GetCharStringCache().GetStringForChar(input->GetString()[startInclusive]);
        }
        else
        {
            subString = SubString::New(input, startInclusive, length);
        }
        ary->DirectAppendItem(subString);
    }

    inline UnifiedRegex::RegexPattern *RegexHelper::GetSplitPattern(ScriptContext* scriptContext, JavascriptRegExp *regularExpression)
    {
        UnifiedRegex::RegexPattern* splitPattern = regularExpression->GetSplitPattern();
        if (!splitPattern)
        {
            UnifiedRegex::RegexPattern* pattern = regularExpression->GetPattern();
            bool isSticky = (pattern->GetFlags() & UnifiedRegex::StickyRegexFlag) != 0;
            if (!isSticky)
            {
                splitPattern = pattern;
            }
            else
            {
                // When the sticky flag is present, the pattern will match the input only at
                // the beginning since "lastIndex" is set to 0 before the first iteration.
                // However, for split(), we need to look for the pattern anywhere in the input.
                //
                // One way to handle this is to use the original pattern with the sticky flag and
                // when it fails, move to the next character and retry.
                //
                // Another way, which is implemented here, is to create another pattern without the
                // sticky flag and have it automatically look for itself anywhere in the input. This
                // way, we can also take advantage of the optimizations for the global search (e.g.,
                // the Boyer-Moore string search).

                InternalString source = pattern->GetSource();
                UnifiedRegex::RegexFlags nonStickyFlags =
                    static_cast<UnifiedRegex::RegexFlags>(pattern->GetFlags() & ~UnifiedRegex::StickyRegexFlag);
                splitPattern = CompileDynamic(
                    scriptContext,
                    source.GetBuffer(),
                    source.GetLength(),
                    nonStickyFlags,
                    pattern->IsLiteral());
            }
            regularExpression->SetSplitPattern(splitPattern);
        }

        return splitPattern;
    }

    Var RegexHelper::RegexSplitImpl(ScriptContext* scriptContext, RecyclableObject* thisObj, JavascriptString* input, CharCount limit, bool noResult, void *const stackAllocationPointer)
    {
        ScriptConfiguration const * scriptConfig = scriptContext->GetConfig();

        if (scriptConfig->IsES6RegExSymbolsEnabled()
            && IsRegexSymbolSplitObservable(thisObj, scriptContext))
        {
            return RegexEs6SplitImpl(scriptContext, thisObj, input, limit, noResult, stackAllocationPointer);
        }
        else
        {
            PCWSTR varName = scriptContext->GetConfig()->IsES6RegExSymbolsEnabled()
                ? _u("RegExp.prototype[Symbol.split]")
                : _u("String.prototype.split");
            JavascriptRegExp* regularExpression = JavascriptRegExp::ToRegExp(thisObj, varName, scriptContext);
            return RegexEs5SplitImpl(scriptContext, regularExpression, input, limit, noResult, stackAllocationPointer);
        }
    }

    bool RegexHelper::IsRegexSymbolSplitObservable(RecyclableObject* instance, ScriptContext* scriptContext)
    {
        DynamicObject* regexPrototype = scriptContext->GetLibrary()->GetRegExpPrototype();
        return !JavascriptRegExp::HasOriginalRegExType(instance)
            || JavascriptRegExp::HasObservableConstructor(regexPrototype)
            || JavascriptRegExp::HasObservableFlags(regexPrototype)
            || JavascriptRegExp::HasObservableExec(regexPrototype);
    }

    Var RegexHelper::RegexEs6SplitImpl(ScriptContext* scriptContext, RecyclableObject* thisObj, JavascriptString* input, CharCount limit, bool noResult, void *const stackAllocationPointer)
    {
        PCWSTR const varName = _u("RegExp.prototype[Symbol.split]");

        JavascriptFunction* defaultConstructor = scriptContext->GetLibrary()->GetRegExpConstructor();
        RecyclableObject* speciesConstructor = JavascriptOperators::SpeciesConstructor(
            thisObj,
            defaultConstructor,
            scriptContext);
        AssertOrFailFast(JavascriptOperators::IsConstructor(speciesConstructor));

        // ES2026 @@split: one observable Get(R, "flags") + ToString with the
        // same substrate fallback @@matchAll makes (without
        // -ES6RegExPrototypeProperties the accessor is absent and a raw Get
        // would stringify undefined, breaking the splitter construction);
        // unicodeMatching is true for "u" OR "v" (v implies the whole
        // u-mode surface).
        JavascriptString* flags = JavascriptRegExp::GetFlagsStringForBuiltin(thisObj, scriptContext);
        bool unicode = wcsstr(flags->GetString(), _u("u")) != nullptr || wcsstr(flags->GetString(), _u("v")) != nullptr;
        flags = AppendStickyToFlagsIfNeeded(flags, scriptContext);

        bool isDefaultConstructor = speciesConstructor == defaultConstructor;
        Var regEx = JavascriptOperators::NewObjectCreationHelper_ReentrancySafe(speciesConstructor, isDefaultConstructor, scriptContext->GetThreadContext(), [=]()->Js::Var
        {
            Js::Var args[] = { speciesConstructor, thisObj, flags };
            Js::CallInfo callInfo(Js::CallFlags_New, _countof(args));
            return JavascriptOperators::NewScObject(
                speciesConstructor,
                Js::Arguments(callInfo, args),
                scriptContext);
        });
        RecyclableObject* splitter = UnsafeVarTo<RecyclableObject>(regEx);

        JavascriptArray* arrayResult = scriptContext->GetLibrary()->CreateArray();

        if (limit == 0)
        {
            return arrayResult;
        }

        CharCount inputLength = input->GetLength();
        if (inputLength == 0)
        {
            Var result = JavascriptRegExp::CallExec(splitter, input, varName, scriptContext);
            if (!JavascriptOperators::IsNull(result))
            {
                return arrayResult;
            }

            arrayResult->DirectAppendItem(input);
            return arrayResult;
        }

        CharCount substringStartIndex = 0; // 'p' in spec
        CharCount substringEndIndex = substringStartIndex; // 'q' in spec
        do // inputLength > 0
        {
            JavascriptRegExp::SetLastIndexProperty(splitter, substringEndIndex, scriptContext);
            Var result = JavascriptRegExp::CallExec(splitter, input, varName, scriptContext); // 'z' in spec
            if (JavascriptOperators::IsNull(result))
            {
                substringEndIndex = AdvanceStringIndex(input, substringEndIndex, unicode);
            }
            else
            {
                CharCount endIndex = JavascriptRegExp::GetLastIndexProperty(splitter, scriptContext); // 'e' in spec
                endIndex = min(endIndex, inputLength);
                if (endIndex == substringStartIndex)
                {
                    substringEndIndex = AdvanceStringIndex(input, substringEndIndex, unicode);
                }
                else
                {
                    AppendSubString(scriptContext, arrayResult, input, substringStartIndex, substringEndIndex);
                    if (arrayResult->GetLength() == limit)
                    {
                        return arrayResult;
                    }

                    substringStartIndex = endIndex;

                    RecyclableObject* resultObject = ExecResultToRecyclableObject(result);

                    int64 length = JavascriptConversion::ToLength(
                        JavascriptOperators::GetProperty(resultObject, PropertyIds::length, scriptContext),
                        scriptContext);
                    uint64 numberOfCaptures = max(length - 1, (int64) 0);
                    for (uint64 i = 1; i <= numberOfCaptures; ++i)
                    {
                        Var nextCapture = JavascriptOperators::GetItem(resultObject, i, scriptContext);
                        arrayResult->DirectAppendItem(nextCapture);

                        if (arrayResult->GetLength() == limit)
                        {
                            return arrayResult;
                        }
                    }

                    substringEndIndex = substringStartIndex;
                }
            }
        }
        while (substringEndIndex < inputLength);

        AppendSubString(scriptContext, arrayResult, input, substringStartIndex, substringEndIndex);

        return arrayResult;
    }

    JavascriptString* RegexHelper::AppendStickyToFlagsIfNeeded(JavascriptString* flags, ScriptContext* scriptContext)
    {
        const char16* flagsString = flags->GetString();
        if (wcsstr(flagsString, _u("y")) == nullptr)
        {
            BEGIN_TEMP_ALLOCATOR(tempAlloc, scriptContext, _u("RegexHelper"))
            {
                StringBuilder<ArenaAllocator> bs(tempAlloc, flags->GetLength() + 1);
                bs.Append(flagsString, flags->GetLength());
                bs.Append(_u('y'));
                flags = Js::JavascriptString::NewCopyBuffer(bs.Detach(), bs.Count(), scriptContext);
            }
            END_TEMP_ALLOCATOR(tempAlloc, scriptContext);
        }

        return flags;
    }

    // String.prototype.split (ES5 15.5.4.14)
    Var RegexHelper::RegexEs5SplitImpl(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input, CharCount limit, bool noResult, void *const stackAllocationPointer)
    {
        if (noResult && scriptContext->GetConfig()->SkipSplitOnNoResult())
        {
            // TODO: Fix this so that the side effect for PropagateLastMatch is done
            return scriptContext->GetLibrary()->GetNull();
        }

#if ENABLE_REGEX_CONFIG_OPTIONS
        RegexHelperTrace(scriptContext, UnifiedRegex::RegexStats::Split, regularExpression, input);
#endif

        JavascriptArray* ary = scriptContext->GetLibrary()->CreateArrayOnStack(stackAllocationPointer);

        if (limit == 0)
        {
            // SPECIAL CASE: Zero limit
            return ary;
        }

        UnifiedRegex::RegexPattern *splitPattern = GetSplitPattern(scriptContext, regularExpression);
        // Hoisted out of the split loop (the empty-match advance reads it per match).
        const bool isUnicodeSplitPattern = RegexIsUnicodePattern(splitPattern);

        const char16* inputStr = input->GetString();
        CharCount inputLength = input->GetLength(); // s in spec
        const int numGroups = splitPattern->NumGroups();
        Var nonMatchValue = NonMatchValue(scriptContext, false);
        UnifiedRegex::GroupInfo lastSuccessfulMatch; // initially undefined

        RegexMatchState state;
        PrimBeginMatch(state, scriptContext, splitPattern, inputStr, inputLength, false);

        if (inputLength == 0)
        {
            // SPECIAL CASE: Empty string
            UnifiedRegex::GroupInfo match = PrimMatch(state, scriptContext, splitPattern, inputLength, 0);
            if (match.IsUndefined())
                ary->DirectAppendItem(input);
            else
                lastSuccessfulMatch = match;
        }
        else
        {
            CharCount copyOffset = 0;  // p in spec
            CharCount startOffset = 0; // q in spec

            CharCount inputLimit = inputLength;

            while (startOffset < inputLimit)
            {
                UnifiedRegex::GroupInfo match = PrimMatch(state, scriptContext, splitPattern, inputLength, startOffset);

                if (match.IsUndefined())
                    break;

                lastSuccessfulMatch = match;

                if (match.offset >= inputLimit)
                    break;

                startOffset = match.offset;
                CharCount endOffset = match.EndOffset(); // e in spec

                if (endOffset == copyOffset)
                {
                    // Empty-match advance: under u/v step a whole code
                    // point so the next attempt cannot start on a pair's
                    // second half (which Matcher::Match would snap back to
                    // this same match). Non-u keeps +1.
                    startOffset = isUnicodeSplitPattern
                        ? RegexAdvanceStringIndexUnicode(inputStr, inputLength, startOffset)
                        : startOffset + 1;
                }
                else
                {
                    AppendSubString(scriptContext, ary, input, copyOffset, startOffset);
                    if (ary->GetLength() >= limit)
                        break;

                    startOffset = copyOffset = endOffset;

                    for (int groupId = 1; groupId < numGroups; groupId++)
                    {
                        ary->DirectAppendItem(GetGroup(scriptContext, splitPattern, input, nonMatchValue, groupId));
                        if (ary->GetLength() >= limit)
                            break;
                    }
                }
            }

            if (ary->GetLength() < limit)
                AppendSubString(scriptContext, ary, input, copyOffset, inputLength);
        }

        PrimEndMatch(state, scriptContext, splitPattern);
        Assert(!splitPattern->IsSticky());
        PropagateLastMatch
            ( scriptContext
            , splitPattern->IsGlobal()
            , /* isSticky */ false
            , regularExpression
            , input
            , lastSuccessfulMatch
            , UnifiedRegex::GroupInfo()
            , /* updateRegex */ true
            , /* updateCtor */ true
            , /* useSplitPattern */ true );

        return ary;
    }

    UnifiedRegex::GroupInfo
    RegexHelper::SimpleMatch(ScriptContext * scriptContext, UnifiedRegex::RegexPattern * pattern, const char16 * input,  CharCount inputLength, CharCount offset)
    {
        RegexMatchState state;
        PrimBeginMatch(state, scriptContext, pattern, input, inputLength, false);
        UnifiedRegex::GroupInfo match = PrimMatch(state, scriptContext, pattern, inputLength, offset);
        PrimEndMatch(state, scriptContext, pattern);
        return match;
    }

    // String.prototype.search (ES5 15.5.4.12)
    Var RegexHelper::RegexSearchImpl(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input)
    {
        UnifiedRegex::RegexPattern* pattern = regularExpression->GetPattern();
        const char16* inputStr = input->GetString();
        CharCount inputLength = input->GetLength();

#if ENABLE_REGEX_CONFIG_OPTIONS
        RegexHelperTrace(scriptContext, UnifiedRegex::RegexStats::Search, regularExpression, input);
#endif
        UnifiedRegex::GroupInfo match = RegexHelper::SimpleMatch(scriptContext, pattern, inputStr, inputLength, 0);

        PropagateLastMatch(scriptContext, pattern->IsGlobal(), pattern->IsSticky(), regularExpression, input, match, match, false, true);

        return JavascriptNumber::ToVar(match.IsUndefined() ? -1 : (int32)match.offset, scriptContext);
    }

    // String.prototype.split (ES5 15.5.4.14)
    Var RegexHelper::StringSplit(JavascriptString* match, JavascriptString* input, CharCount limit)
    {
        ScriptContext* scriptContext = match->GetScriptContext();
        JavascriptArray* ary;
        CharCount matchLen = match->GetLength();
        if (matchLen == 0)
        {
            CharCount count = min(input->GetLength(), limit);
            ary = scriptContext->GetLibrary()->CreateArray(count);
            const char16 * charString = input->GetString();
            for (CharCount i = 0; i < count; i++)
            {
                ary->DirectSetItemAt(i, scriptContext->GetLibrary()->GetCharStringCache().GetStringForChar(charString[i]));
            }
        }
        else
        {
            CharCount i = 0;
            CharCount offset = 0;
            ary = scriptContext->GetLibrary()->CreateArray(0);
            while (i < limit)
            {
                CharCount prevOffset = offset;
                offset = JavascriptString::strstr(input, match, false, prevOffset);
                if (offset != CharCountFlag)
                {
                    ary->DirectSetItemAt(i++, SubString::New(input, prevOffset, offset-prevOffset));
                    offset += max(matchLen, static_cast<CharCount>(1));
                    if (offset > input->GetLength())
                        break;
                }
                else
                {
                    ary->DirectSetItemAt(i++, SubString::New(input, prevOffset, input->GetLength() - prevOffset));
                    break;
                }
            }
        }
        return ary;
    }

    bool RegexHelper::IsResultNotUsed(CallFlags flags)
    {
        return !PHASE_OFF1(Js::RegexResultNotUsedPhase) && ((flags & CallFlags_NotUsed) != 0);
    }

    // ----------------------------------------------------------------------
    // Primitives
    // ----------------------------------------------------------------------
    void RegexHelper::PrimBeginMatch(RegexMatchState& state, ScriptContext* scriptContext, UnifiedRegex::RegexPattern* pattern, const char16* input, CharCount inputLength, bool alwaysNeedAlloc)
    {
        state.input = input;
        if (pattern->rep.unified.matcher == 0)
            pattern->rep.unified.matcher = UnifiedRegex::Matcher::New(scriptContext, pattern);
        if (alwaysNeedAlloc)
            state.tempAllocatorObj = scriptContext->GetTemporaryAllocator(_u("RegexUnifiedExecTemp"));
        else
            state.tempAllocatorObj = 0;
    }

    UnifiedRegex::GroupInfo
    RegexHelper::PrimMatch(RegexMatchState& state, ScriptContext* scriptContext, UnifiedRegex::RegexPattern* pattern, CharCount inputLength, CharCount offset)
    {
        Assert(pattern->rep.unified.program != 0);
        Assert(pattern->rep.unified.matcher != 0);
#if ENABLE_REGEX_CONFIG_OPTIONS
        UnifiedRegex::RegexStats* stats = 0;
        if (REGEX_CONFIG_FLAG(RegexProfile))
        {
            stats = scriptContext->GetRegexStatsDatabase()->GetRegexStats(pattern);
            scriptContext->GetRegexStatsDatabase()->BeginProfile();
        }
        UnifiedRegex::DebugWriter* w = 0;
        if (REGEX_CONFIG_FLAG(RegexTracing) && CONFIG_FLAG(Verbose))
            w = scriptContext->GetRegexDebugWriter();
#endif

        pattern->rep.unified.matcher->Match
            (state.input
                , inputLength
                , offset
                , scriptContext
#if ENABLE_REGEX_CONFIG_OPTIONS
                , stats
                , w
#endif
                );

#if ENABLE_REGEX_CONFIG_OPTIONS
        if (REGEX_CONFIG_FLAG(RegexProfile))
            scriptContext->GetRegexStatsDatabase()->EndProfile(stats, UnifiedRegex::RegexStats::Execute);
#endif
        return pattern->GetGroup(0);
    }

    void RegexHelper::PrimEndMatch(RegexMatchState& state, ScriptContext* scriptContext, UnifiedRegex::RegexPattern* pattern)
    {
        if (state.tempAllocatorObj != 0)
            scriptContext->ReleaseTemporaryAllocator(state.tempAllocatorObj);
    }

    Var RegexHelper::NonMatchValue(ScriptContext* scriptContext, bool isGlobalCtor)
    {
        // SPEC DEVIATION: The $n properties of the RegExp ctor use empty strings rather than undefined to represent
        //                 the non-match value, even in ES5 mode.
        if (isGlobalCtor)
            return scriptContext->GetLibrary()->GetEmptyString();
        else
            return scriptContext->GetLibrary()->GetUndefined();
    }

    Var RegexHelper::GetString(ScriptContext* scriptContext, JavascriptString* input, Var nonMatchValue, UnifiedRegex::GroupInfo group)
    {
        if (group.IsUndefined())
            return nonMatchValue;
        switch (group.length)
        {
        case 0:
            return scriptContext->GetLibrary()->GetEmptyString();
        case 1:
        {
            const char16* inputStr = input->GetString();
            return scriptContext->GetLibrary()->GetCharStringCache().GetStringForChar(inputStr[group.offset]);
        }
        case 2:
        {
            const char16* inputStr = input->GetString();
            PropertyString* propString = scriptContext->GetPropertyString2(inputStr[group.offset], inputStr[group.offset + 1]);
            if (propString != 0)
                return propString;
            // fall-through for default
        }
        default:
            return SubString::New(input, group.offset, group.length);
        }
    }

    Var RegexHelper::GetGroup(ScriptContext* scriptContext, UnifiedRegex::RegexPattern* pattern, JavascriptString* input, Var nonMatchValue, int groupId)
    {
        return GetString(scriptContext, input, nonMatchValue, pattern->GetGroup(groupId));
    }

    DynamicObject* RegexHelper::CreateGroupsObject(ScriptContext* scriptContext, UnifiedRegex::RegexPattern* pattern, JavascriptString* input)
    {
        UnifiedRegex::Program* program = pattern->rep.unified.program;
        Assert(program->HasNamedGroups());
        Assert(pattern->WasLastMatchSuccessful());

        DynamicObject* groups = scriptContext->GetLibrary()->CreateObject(
            scriptContext->GetLibrary()->GetNull()); // null [[Prototype]]
        Var undefined = scriptContext->GetLibrary()->GetUndefined();
        const uint32 numGroupNames = program->numGroupNames;
        for (uint32 i = 0; i < numGroupNames; i++)
        {
            CharCount nameLen = 0;
            const char16* name = program->GetGroupName(i, &nameLen);
            PropertyId propertyId = scriptContext->GetOrAddPropertyIdTracked(name, static_cast<int>(nameLen));
            // ES2025 duplicate named groups: a later entry for the same name
            // resolves to the same participating group, so the extra set is
            // an idempotent overwrite and the property keeps its
            // first-occurrence (textual) position. No group with the name
            // participating -> undefined.
            const int groupId = ResolveNamedGroupId(program, pattern, name, nameLen);
            Var value = groupId < 0 ? undefined
                : GetString(scriptContext, input, undefined, pattern->GetGroup(groupId));
            // CreateDataProperty semantics: on a null-prototype object the
            // ordinary [[Set]] creates an own writable/enumerable/configurable
            // data property (and "__proto__" is not special without a prototype).
            JavascriptOperators::SetProperty(groups, groups, propertyId, value, scriptContext);
        }
        return groups;
    }

    int RegexHelper::ResolveNamedGroupId(UnifiedRegex::Program* program, UnifiedRegex::RegexPattern* pattern, const char16* name, CharCount nameLen)
    {
        int result = -1;
        const uint32 numGroupNames = program->numGroupNames;
        for (uint32 i = 0; i < numGroupNames; i++)
        {
            CharCount candidateLen = 0;
            const char16* candidate = program->GetGroupName(i, &candidateLen);
            if (candidateLen != nameLen)
                continue;
            CharCount j = 0;
            while (j < nameLen && candidate[j] == name[j])
                j++;
            if (j != nameLen)
                continue;
            // Entries are in textual (= numeric) order; duplicates span
            // only a disjunction and loop bodies reset their captures each
            // iteration, so the numeric-order-last participant is the last
            // participant.
            if (!pattern->GetGroup(program->GetGroupNameGroupId(i)).IsUndefined())
                result = program->GetGroupNameGroupId(i);
        }
        return result;
    }

    Var RegexHelper::GetGroupsVar(ScriptContext* scriptContext, UnifiedRegex::RegexPattern* pattern, JavascriptString* input)
    {
        if (!pattern->rep.unified.program->HasNamedGroups())
        {
            return scriptContext->GetLibrary()->GetUndefined();
        }
        return CreateGroupsObject(scriptContext, pattern, input);
    }

    JavascriptArray* RegexHelper::CreateIndicesArray(ScriptContext* scriptContext, UnifiedRegex::RegexPattern* pattern, JavascriptString* input)
    {
        Assert(pattern->IsHasIndices());
        Assert(pattern->WasLastMatchSuccessful());

        JavascriptLibrary* library = scriptContext->GetLibrary();
        const int numGroups = pattern->NumGroups();
        Var undefined = library->GetUndefined();

        // A real Array (ArrayCreate(n)) with one [start, end] pair per
        // capture, in capture order starting at capture 0 (the whole match);
        // undefined slots for groups that did not participate.
        JavascriptArray* indices = library->CreateArray(numGroups);
        for (int groupId = 0; groupId < numGroups; groupId++)
        {
            UnifiedRegex::GroupInfo group = pattern->GetGroup(groupId);
            if (group.IsUndefined())
            {
                indices->DirectSetItemAt(groupId, undefined);
                continue;
            }
            JavascriptArray* matchIndices = library->CreateArray(2);
            matchIndices->DirectSetItemAt(0, JavascriptNumber::ToVar(group.offset, scriptContext));
            matchIndices->DirectSetItemAt(1, JavascriptNumber::ToVar(group.offset + group.length, scriptContext));
            indices->DirectSetItemAt(groupId, matchIndices);
        }

        // Own "groups" data property: the property always exists (spec
        // MakeIndicesArray creates it unconditionally); its value is a
        // null-prototype object mirroring the named groups' pairs when the
        // pattern has named captures, undefined otherwise. CreateDataProperty
        // semantics: an ordinary [[Set]] here would be observable through
        // Array.prototype setters (corpus indices-groups-object.js).
        UnifiedRegex::Program* program = pattern->rep.unified.program;
        const bool hasNamedGroups = program->HasNamedGroups();
        Var groupsVar = undefined;
        if (hasNamedGroups)
        {
            DynamicObject* groups = library->CreateObject(library->GetNull()); // null [[Prototype]]
            const uint32 numGroupNames = program->numGroupNames;
            for (uint32 i = 0; i < numGroupNames; i++)
            {
                CharCount nameLen = 0;
                const char16* name = program->GetGroupName(i, &nameLen);
                PropertyId propertyId = scriptContext->GetOrAddPropertyIdTracked(name, static_cast<int>(nameLen));
                // Same duplicate-name rule as CreateGroupsObject: the last
                // participating group with this name supplies the pair
                // (idempotent for repeated entries); undefined when none.
                // MakeIndicesArray creates one matchIndexPair and defines that
                // same object on both indices[i] and indices.groups. The pair
                // was stored in the loop above; a second array would break
                // identity (and mutation) with the numbered slot.
                const int groupId = ResolveNamedGroupId(program, pattern, name, nameLen);
                Var value = undefined;
                if (groupId >= 0)
                {
                    Var stored = nullptr;
                    if (indices->DirectGetItemAt(static_cast<uint32>(groupId), &stored))
                    {
                        value = stored;
                    }
                    else
                    {
                        Assert(false);
                    }
                }
                // CreateDataProperty semantics on a null-prototype object
                // (see CreateGroupsObject), including for "__proto__" names.
                JavascriptOperators::SetProperty(groups, groups, propertyId, value, scriptContext);
            }
            groupsVar = groups;
        }
        indices->SetPropertyWithAttributes(
            library->GetRegexGroupsPropertyRecord()->GetPropertyId(),
            groupsVar,
            PropertyDynamicTypeDefaults,
            nullptr,
            PropertyOperation_None);

        return indices;
    }

    // ======================================================================
    // Match results propagate into three places:
    //  - The match result array. Generally the array has string entries for the overall match substring,
    //    followed by final bindings for each group, plus the fields:
    //     - 'input': string used in match
    //     - 'index': index of first character of match in input
    //     - 'lastIndex' (IE extension): one plus index of last character of match in input
    //    However, for String.match with a global match, the result is an array of all match results
    //    (ignoring any group bindings). But in IE8 mode we also bind the above fields to that array,
    //    using the results of the last successful primitive match.
    //  - The regular expression object has writable field:
    //     - 'lastIndex': one plus index of last character of last match in last input
    //     - 'lastInput
    //  - (Host extension) The RegExp constructor object has fields:
    //     - '$n': last match substrings, using "" for undefined in all modes
    //     - etc (see JavascriptRegExpConstructorType.cpp)
    //
    // There are also three influences on what gets propagated where and when:
    //  - Whether the regular expression is global
    //  - Whether the primitive operations runs the regular expression until failure (e.g. String.match) or
    //    just once (e.g. RegExp.exec), or use the underlying matching machinery implicitly (e.g. String.split).
    //
    // Here are the rules:
    //  - RegExp is updated for the last *successful* primitive match, except for String.replace.
    //    In particular, for String.match with a global regex, the final failing match *does not* reset RegExp.
    //  - Except for String.search in EC5 mode (which does not update 'lastIndex'), the regular expressions
    //    lastIndex is updated as follows:
    //     - ES5 mode, if a primitive match fails then the regular expression 'lastIndex' is set to 0. In particular,
    //       the final failing primitive match for String.match with a global regex forces 'lastIndex' to be reset.
    //       However, if a primitive match succeeds then the regular expression 'lastIndex' is updated only for
    //       a global regex.
    //       for success. However:
    //        - The last failing match in a String.match with a global regex does NOT reset 'lastIndex'.
    //        - If the regular expression matched empty, the last index is set assuming the pattern actually matched
    //          one input character. This applies even if the pattern matched empty one beyond the end of the string
    //          in a String.match with a global regex (!). For our own sanity, we isolate this particular case
    //          within JavascriptRegExp when setting the lastIndexVar value.
    //  - In all modes, 'lastIndex' determines the starting search index only for global regular expressions.
    //
    // ======================================================================

    void RegexHelper::PropagateLastMatch
        ( ScriptContext* scriptContext
        , bool isGlobal
        , bool isSticky
        , JavascriptRegExp* regularExpression
        , JavascriptString* lastInput
        , UnifiedRegex::GroupInfo lastSuccessfulMatch
        , UnifiedRegex::GroupInfo lastActualMatch
        , bool updateRegex
        , bool updateCtor
        , bool useSplitPattern )
    {
        if (updateRegex)
        {
            PropagateLastMatchToRegex(scriptContext, isGlobal, isSticky, regularExpression, lastSuccessfulMatch, lastActualMatch);
        }
        if (updateCtor)
        {
            PropagateLastMatchToCtor(scriptContext, regularExpression, lastInput, lastSuccessfulMatch, useSplitPattern);
        }
    }

    void RegexHelper::PropagateLastMatchToRegex
        ( ScriptContext* scriptContext
        , bool isGlobal
        , bool isSticky
        , JavascriptRegExp* regularExpression
        , UnifiedRegex::GroupInfo lastSuccessfulMatch
        , UnifiedRegex::GroupInfo lastActualMatch )
    {
        if (scriptContext->GetConfig()->IsES6RegExSymbolsEnabled())
        {
            // ES2018+ RegExpBuiltinExec semantics: "lastIndex" is written
            // through the observable [[Set]] (Set(R, "lastIndex", …, true)),
            // so a non-writable or accessor "lastIndex" throws or runs user
            // code, and a failed match of a non-global, non-sticky regexp
            // does not touch it at all. See doc/c2-s7-matchall.md.
            if (lastActualMatch.IsUndefined())
            {
                if (isGlobal || isSticky)
                {
                    JavascriptRegExp::SetLastIndexProperty(regularExpression, TaggedInt::ToVarUnchecked(0), scriptContext);
                }
            }
            else if (isGlobal || isSticky)
            {
                CharCount lastIndex = lastActualMatch.EndOffset();
                Assert(lastIndex <= MaxCharCount);
                JavascriptRegExp::SetLastIndexProperty(regularExpression, (int32)lastIndex, scriptContext);
            }
            return;
        }

        if (lastActualMatch.IsUndefined())
        {
            regularExpression->SetLastIndex(0);
        }
        else if (isGlobal || isSticky)
        {
            CharCount lastIndex = lastActualMatch.EndOffset();
            Assert(lastIndex <= MaxCharCount);
            regularExpression->SetLastIndex((int32)lastIndex);
        }
    }

    void RegexHelper::PropagateLastMatchToCtor
        ( ScriptContext* scriptContext
        , JavascriptRegExp* regularExpression
        , JavascriptString* lastInput
        , UnifiedRegex::GroupInfo lastSuccessfulMatch
        , bool useSplitPattern )
    {
        Assert(lastInput);

        if (!lastSuccessfulMatch.IsUndefined())
        {
            // Notes:
            // - SPEC DEVIATION: The RegExp ctor holds some details of the last successful match on any regular expression.
            // - For updating regex ctor's stats we are using entry function's context, rather than regex context,
            //   the rational is: use same context of RegExp.prototype, on which the function was called.
            //   So, if you call the function with remoteContext.regexInstance.exec.call(localRegexInstance, "match string"),
            //   we will update stats in the context related to the exec function, i.e. remoteContext.
            //   This is consistent with other browsers
            UnifiedRegex::RegexPattern* pattern = useSplitPattern
                ? regularExpression->GetSplitPattern()
                : regularExpression->GetPattern();
            scriptContext->GetLibrary()->GetRegExpConstructor()->SetLastMatch(pattern, lastInput, lastSuccessfulMatch);
        }
    }

    void RegexHelper::InvalidateLastMatchOnCtor(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* lastInput, bool useSplitPattern)
    {
        Assert(lastInput);

        UnifiedRegex::RegexPattern* pattern = useSplitPattern
            ? regularExpression->GetSplitPattern()
            : regularExpression->GetPattern();
        scriptContext->GetLibrary()->GetRegExpConstructor()->InvalidateLastMatch(pattern, lastInput);
    }

    bool RegexHelper::GetInitialOffset(bool isGlobal, bool isSticky, JavascriptRegExp* regularExpression, CharCount inputLength, CharCount& offset, ScriptContext* scriptContext)
    {
        if (isGlobal || isSticky)
        {
            offset = regularExpression->GetLastIndex();
            if (offset <= MaxCharCount)
                return true;
            else
            {
                // "lastIndex" only reaches here clamped above MaxCharCount,
                // so it is greater than any input length: RegExpBuiltinExec
                // resets it through the observable Set under ES6RegExSymbols
                // (a non-writable "lastIndex" throws). A non-global,
                // non-sticky regexp never takes this path, so its "lastIndex"
                // is never touched here. Flag-off keeps the internal write.
                if (scriptContext->GetConfig()->IsES6RegExSymbolsEnabled())
                {
                    JavascriptRegExp::SetLastIndexProperty(regularExpression, TaggedInt::ToVarUnchecked(0), scriptContext);
                }
                else
                {
                    regularExpression->SetLastIndex(0);
                }
                return false;
            }
        }
        else
        {
            offset = 0;
            return true;
        }
    }

    JavascriptArray* RegexHelper::CreateMatchResult(void *const stackAllocationPointer, ScriptContext* scriptContext, bool isGlobal, int numGroups, JavascriptString* input, bool hasIndices)
    {
        if (isGlobal)
        {
            // Use an ordinary array, with default initial capacity
            return scriptContext->GetLibrary()->CreateArrayOnStack(stackAllocationPointer);
        }
        else
            return JavascriptRegularExpressionResult::Create(stackAllocationPointer, numGroups, input, scriptContext, hasIndices);
    }

    void RegexHelper::FinalizeMatchResult(ScriptContext* scriptContext, bool isGlobal, JavascriptArray* arr, UnifiedRegex::GroupInfo match)
    {
        if (!isGlobal)
            JavascriptRegularExpressionResult::SetMatch(arr, match);
        // else: arr is an ordinary array
    }

    JavascriptArray* RegexHelper::CreateExecResult(void *const stackAllocationPointer, ScriptContext* scriptContext, int numGroups, JavascriptString* input, UnifiedRegex::GroupInfo match, bool hasIndices)
    {
        JavascriptArray* res = JavascriptRegularExpressionResult::Create(stackAllocationPointer, numGroups, input, scriptContext, hasIndices);
        JavascriptRegularExpressionResult::SetMatch(res, match);
        return res;
    }

    template<bool mustMatchEntireInput>
    BOOL RegexHelper::RegexTest_NonScript(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, const char16 *const input, const CharCount inputLength)
    {
        // This version of the function should only be used when testing the regex against a non-javascript string. That is,
        // this call was not initiated by script code. Hence, the RegExp constructor is not updated with the last match. If
        // 'mustMatchEntireInput' is true, this function also ignores the global/sticky flag and the lastIndex property, since it tests
        // for a match on the entire input string; in that case, the lastIndex property is not modified.

        UnifiedRegex::RegexPattern* pattern = regularExpression->GetPattern();
        UnifiedRegex::GroupInfo match; // initially undefined

#if ENABLE_REGEX_CONFIG_OPTIONS
        RegexHelperTrace(scriptContext, UnifiedRegex::RegexStats::Test, regularExpression, input, inputLength);
#endif
        const bool isGlobal = pattern->IsGlobal();
        const bool isSticky = pattern->IsSticky();
        CharCount offset;
        if (mustMatchEntireInput)
            offset = 0; // needs to match the entire input, so ignore 'lastIndex' and always start from the beginning
        else if (!GetInitialOffset(isGlobal, isSticky, regularExpression, inputLength, offset, scriptContext))
            return false;

        if (mustMatchEntireInput || offset <= inputLength)
        {
            match = RegexHelper::SimpleMatch(scriptContext, pattern, input, inputLength, offset);
        }
        // else: match remains undefined

        if (!mustMatchEntireInput) // don't update 'lastIndex' when mustMatchEntireInput is true since the global flag is ignored
        {
            PropagateLastMatchToRegex(scriptContext, isGlobal, isSticky, regularExpression, match, RegexAdjustedLastMatchForPropagation(pattern, input, inputLength, match));
        }

        return mustMatchEntireInput ? match.offset == 0 && match.length == inputLength : !match.IsUndefined();
    }

    // explicit instantiation
    template BOOL RegexHelper::RegexTest_NonScript<true>(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, const char16 *const input, const CharCount inputLength);
    template BOOL RegexHelper::RegexTest_NonScript<false>(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, const char16 *const input, const CharCount inputLength);

    // Asserts if the value needs to be marshaled to target context.
    // Returns the resulting value.
    // This is supposed to be called for result/return value of the RegexXXX functions.
    // static
    template<typename T>
    T RegexHelper::CheckCrossContextAndMarshalResult(T value, ScriptContext* targetContext)
    {
        Assert(targetContext);
        Assert(!CrossSite::NeedMarshalVar(value, targetContext));
        return value;
    }

    Var RegexHelper::RegexMatchResultUsed(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input)
    {
        return RegexHelper::RegexMatch(scriptContext, regularExpression, input, false);
    }

    Var RegexHelper::RegexMatchResultUsedAndMayBeTemp(void *const stackAllocationPointer, ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input)
    {
        return RegexHelper::RegexMatch(scriptContext, regularExpression, input, false, stackAllocationPointer);
    }

    Var RegexHelper::RegexMatchResultNotUsed(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input)
    {
        if (!PHASE_OFF1(Js::RegexResultNotUsedPhase))
        {
            return RegexHelper::RegexMatch(scriptContext, regularExpression, input, true);
        }
        else
        {
            return RegexHelper::RegexMatch(scriptContext, regularExpression, input, false);
        }
    }

    Var RegexHelper::RegexMatch(ScriptContext* entryFunctionContext, RecyclableObject *thisObj, JavascriptString *input, bool noResult, void *const stackAllocationPointer)
    {
        Var result = RegexHelper::RegexMatchImpl<true>(entryFunctionContext, thisObj, input, noResult, stackAllocationPointer);
        return RegexHelper::CheckCrossContextAndMarshalResult(result, entryFunctionContext);
    }

    Var RegexHelper::RegexMatchNoHistory(ScriptContext* entryFunctionContext, JavascriptRegExp *regularExpression, JavascriptString *input, bool noResult)
    {
        // RegexMatchNoHistory() is used only by Intl internally and there is no need for ES6
        // observable RegExp actions. Therefore, we can directly use the ES5 logic.
        Var result = RegexHelper::RegexEs5MatchImpl<false>(entryFunctionContext, regularExpression, input, noResult);
        return RegexHelper::CheckCrossContextAndMarshalResult(result, entryFunctionContext);
    }

    Var RegexHelper::RegexExecResultUsed(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input)
    {
        return RegexHelper::RegexExec(scriptContext, regularExpression, input, false);
    }

    Var RegexHelper::RegexExecResultUsedAndMayBeTemp(void *const stackAllocationPointer, ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input)
    {
        return RegexHelper::RegexExec(scriptContext, regularExpression, input, false, stackAllocationPointer);
    }

    Var RegexHelper::RegexExecResultNotUsed(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input)
    {
        if (!PHASE_OFF1(Js::RegexResultNotUsedPhase))
        {
            return RegexHelper::RegexExec(scriptContext, regularExpression, input, true);
        }
        else
        {
            return RegexHelper::RegexExec(scriptContext, regularExpression, input, false);
        }
    }

    Var RegexHelper::RegexExec(ScriptContext* entryFunctionContext, JavascriptRegExp* regularExpression, JavascriptString* input, bool noResult, void *const stackAllocationPointer)
    {
        Var result = RegexHelper::RegexExecImpl(entryFunctionContext, regularExpression, input, noResult, stackAllocationPointer);
        return RegexHelper::CheckCrossContextAndMarshalResult(result, entryFunctionContext);
    }

    Var RegexHelper::RegexReplaceResultUsed(ScriptContext* entryFunctionContext, JavascriptRegExp* regularExpression, JavascriptString* input, JavascriptString* replace)
    {
        return entryFunctionContext->GetConfig()->IsES6RegExSymbolsEnabled()
            ? RegexHelper::RegexReplace(entryFunctionContext, regularExpression, input, replace, false)
            : RegexHelper::RegexEs5Replace(entryFunctionContext, regularExpression, input, replace, false);
    }

    Var RegexHelper::RegexReplaceResultNotUsed(ScriptContext* entryFunctionContext, JavascriptRegExp* regularExpression, JavascriptString* input, JavascriptString* replace)
    {
        if (!PHASE_OFF1(Js::RegexResultNotUsedPhase))
        {
            return entryFunctionContext->GetConfig()->IsES6RegExSymbolsEnabled()
                ? RegexHelper::RegexReplace(entryFunctionContext, regularExpression, input, replace, true)
                : RegexHelper::RegexEs5Replace(entryFunctionContext, regularExpression, input, replace, true);
        }
        else
        {
            return entryFunctionContext->GetConfig()->IsES6RegExSymbolsEnabled()
                ? RegexHelper::RegexReplace(entryFunctionContext, regularExpression, input, replace, false)
                : RegexHelper::RegexEs5Replace(entryFunctionContext, regularExpression, input, replace, false);
        }

    }

    Var RegexHelper::RegexReplace(ScriptContext* entryFunctionContext, RecyclableObject* thisObj, JavascriptString* input, JavascriptString* replace, bool noResult)
    {
        Var result = RegexHelper::RegexReplaceImpl(entryFunctionContext, thisObj, input, replace, noResult);
        return RegexHelper::CheckCrossContextAndMarshalResult(result, entryFunctionContext);
    }

    Var RegexHelper::RegexEs5Replace(ScriptContext* entryFunctionContext, JavascriptRegExp* regularExpression, JavascriptString* input, JavascriptString* replace, bool noResult)
    {
        // We can have RegexReplaceResult... functions defer their job to RegexReplace. However, their regularExpression argument
        // would first be cast to RecyclableObject when the call is made, and then back to JavascriptRegExp in RegexReplaceImpl.
        // The conversion back slows down the perf, so we use this ES5 version of RegexReplace in RegexReplaceResult... if we know
        // that the ES6 logic isn't needed.

        Var result = RegexHelper::RegexEs5ReplaceImpl(entryFunctionContext, regularExpression, input, replace, noResult);
        return RegexHelper::CheckCrossContextAndMarshalResult(result, entryFunctionContext);
    }

    Var RegexHelper::RegexReplaceFunction(ScriptContext* entryFunctionContext, RecyclableObject* thisObj, JavascriptString* input, RecyclableObject* replacefn)
    {
        Var result = RegexHelper::RegexReplaceImpl(entryFunctionContext, thisObj, input, replacefn);
        return RegexHelper::CheckCrossContextAndMarshalResult(result, entryFunctionContext);
    }

    Var RegexHelper::RegexSearch(ScriptContext* entryFunctionContext, JavascriptRegExp* regularExpression, JavascriptString* input)
    {
        Var result = RegexHelper::RegexSearchImpl(entryFunctionContext, regularExpression, input);
        return RegexHelper::CheckCrossContextAndMarshalResult(result, entryFunctionContext);
    }

    Var RegexHelper::RegexSplitResultUsed(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input, CharCount limit)
    {
        return RegexHelper::RegexSplit(scriptContext, regularExpression, input, limit, false);
    }

    Var RegexHelper::RegexSplitResultUsedAndMayBeTemp(void *const stackAllocationPointer, ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input, CharCount limit)
    {
        Assert(ThreadContext::IsOnStack(stackAllocationPointer));

        return RegexHelper::RegexSplit(scriptContext, regularExpression, input, limit, false, stackAllocationPointer);
    }

    Var RegexHelper::RegexSplitResultNotUsed(ScriptContext* scriptContext, JavascriptRegExp* regularExpression, JavascriptString* input, CharCount limit)
    {
        if (!PHASE_OFF1(Js::RegexResultNotUsedPhase))
        {
            return RegexHelper::RegexSplit(scriptContext, regularExpression, input, limit, true);
        }
        else
        {
            return RegexHelper::RegexSplit(scriptContext, regularExpression, input, limit, false);
        }
    }

    Var RegexHelper::RegexSplit(ScriptContext* entryFunctionContext, RecyclableObject* thisObj, JavascriptString* input, CharCount limit, bool noResult, void *const stackAllocationPointer)
    {
        Var result = RegexHelper::RegexSplitImpl(entryFunctionContext, thisObj, input, limit, noResult, stackAllocationPointer);
        return RegexHelper::CheckCrossContextAndMarshalResult(result, entryFunctionContext);
    }

    RecyclableObject* RegexHelper::ExecResultToRecyclableObject(Var result)
    {
        // "result" is the result of the "exec" call. "CallExec" makes sure that it is either
        // an Object or Null. RegExp algorithms have special conditions for when the result is Null,
        // so we can directly cast to RecyclableObject.
        Assert(!JavascriptOperators::IsNull(result));
        return UnsafeVarTo<RecyclableObject>(result);
    }

    JavascriptString* RegexHelper::GetMatchStrFromResult(RecyclableObject* result, ScriptContext* scriptContext)
    {
        return JavascriptConversion::ToString(
            JavascriptOperators::GetItem(result, (uint32)0, scriptContext),
            scriptContext);
    }

    void RegexHelper::AdvanceLastIndex(
        RecyclableObject* instance,
        JavascriptString* input,
        JavascriptString* matchStr,
        bool unicode,
        ScriptContext* scriptContext)
    {
        if (matchStr->GetLength() == 0)
        {
            CharCount lastIndex = JavascriptRegExp::GetLastIndexProperty(instance, scriptContext);
            lastIndex = AdvanceStringIndex(input, lastIndex, unicode);
            JavascriptRegExp::SetLastIndexProperty(instance, lastIndex, scriptContext);
        }
    }

    CharCount RegexHelper::AdvanceStringIndex(JavascriptString* string, CharCount index, bool isUnicode)
    {
        // ES2026 22.2.7.3: under unicode the index advances by the code
        // point's code-unit count, so a position at the start of a complete
        // surrogate pair skips both units (empty-match iteration in
        // @@match/@@replace/matchAll). Non-unicode advances by one unit.
        if (!isUnicode)
        {
            return JavascriptRegExp::AddIndex(index, 1);
        }

        const CharCount length = string->GetLength();
        if (index >= length)
        {
            return JavascriptRegExp::AddIndex(index, 1);
        }
        const char16* const input = string->GetString();
        return JavascriptRegExp::AddIndex(
            index,
            RegexAdvanceStringIndexUnicode(input, length, index) - index);
    }
}
