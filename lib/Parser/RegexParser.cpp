//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*
--------------------------------------------------------------------------------------------------------------------------------
Original
--------------------------------------------------------------------------------------------------------------------------------

Pattern ::= Disjunction
Disjunction ::= Alternative | Alternative '|' Disjunction
Alternative ::= [empty] | Alternative Term
Term ::= Assertion | Atom | Atom Quantifier
Assertion ::= '^' | '$' | '\' 'b' | '\' 'B' | '(' '?' '=' Disjunction ')' | '(' '?' '!' Disjunction ')'
Quantifier ::= QuantifierPrefix | QuantifierPrefix '?'
QuantifierPrefix ::= '*' | '+' | '?' | '{' DecimalDigits '}' | '{' DecimalDigits ',' '}' | '{' DecimalDigits ',' DecimalDigits '}'
Atom ::= PatternCharacter | '.' | '\' AtomEscape | CharacterClass | '(' Disjunction ')' | '(' '?' ':' Disjunction ')'
PatternCharacter ::= SourceCharacter but not any of { '^', '$', '\', '.', '*', '+', '?', '(', ')', '[', ']', '{', '}', '|' }
AtomEscape ::= DecimalEscape | CharacterEscape | CharacterClassEscape
CharacterEscape ::= ControlEscape | 'c' ControlLetter | HexEscapeSequence | UnicodeEscapeSequence | IdentityEscape
ControlEscape ::= one of { 'f', 'n', 'r', 't', 'v' }
ControlLetter ::= one of { 'a'..'z', 'A'..'Z' }
IdentityEscape ::= (SourceCharacter but not IdentifierPart) | <ZWJ> | <ZWNJ>
DecimalEscape ::= DecimalIntegerLiteral [lookahead not in DecimalDigit]
CharacterClassEscape :: one of { 'd', 'D', 's', 'S', 'w', 'W' }
CharacterClass ::= '[' [lookahead not in {'^'}] ClassRanges ']' | '[' '^' ClassRanges ']'
ClassRanges ::= [empty] | NonemptyClassRanges
NonemptyClassRanges ::= ClassAtom | ClassAtom NonemptyClassRangesNoDash | ClassAtom '-' ClassAtom ClassRanges
NonemptyClassRangesNoDash ::= ClassAtom | ClassAtomNoDash NonemptyClassRangesNoDash | ClassAtomNoDash '-' ClassAtom ClassRanges
ClassAtom ::= '-' | ClassAtomNoDash
ClassAtomNoDash ::= SourceCharacter but not one of { '\', ']', '-' } | '\' ClassEscape
ClassEscape ::= DecimalEscape | 'b' | CharacterEscape | CharacterClassEscape
SourceCharacter ::= <unicode character>
HexEscapeSequence ::= 'x' HexDigit HexDigit
UnicodeEscapeSequence ::= 'u' HexDigit HexDigit HexDigit HexDigit | 'u' '{' HexDigits '}'
HexDigit ::= one of { '0'..'9', 'a'..'f', 'A'..'F' }
IdentifierStart ::= UnicodeLetter | '$' | '_' | '\' UnicodeEscapeSequence
IdentifierPart ::= IdentifierStart | UnicodeCombiningMark | UnicodeDigit | UnicodeConnectorPunctuation | <ZWNJ> | <ZWJ>
UnicodeLetter ::= <unicode Uppercase letter> | <unicode Lowercase letter> | <unicode Titlecase letter> | <unicode Modifier letter> | <unicode Other letter> | <unicode Letter number>
UnicodeCombiningMark = <unicode Non-spacing mark> | <unicode combining spacing mark>
UnicodeDigit ::= <unicode Decimal number>
UnicodeConnectorPunctuation ::= <unicode Connector punctuation>
DecimalIntegerLiteral ::= '0' | NonZeroDigit DecimalDigits?
DecimalDigits ::= DecimalDigit | DecimalDigits DecimalDigit
DecimalDigit ::= one of { '0'..'9' }
NonZeroDigit ::= one of { '1'..'9' }

------------------
Annex B Deviations
------------------

1. The assertions (?= ) and (?! ) are treated as though they have a surrounding non-capture group, and hence can be quantified.
   Other assertions are not quantifiable.

QuantifiableAssertion [added] ::= '(' '?' '=' Disjunction ')' | '(' '?' '!' Disjunction ')'
Assertion [replaced] ::= '^' | '$' | '\' 'b' | '\' 'B' | QuantifiableAssertion
Term ::= ... | QuantifiableAssertion Quantifier [added]

--------------------------------------------------------------------------------------------------------------------------------
Left factored
--------------------------------------------------------------------------------------------------------------------------------

Pattern ::= Disjunction
Disjunction ::= Alternative ('|' Alternative)*
    FOLLOW(Disjunction) = { <eof>, ')' }
Alternative ::= Term*
    FOLLOW(Alternative) = { <eof>, ')', '|' }
Term ::= ( '^' | '$' | '\' 'b' | '\' 'B' | '(' '?' '=' Disjunction ')' | '(' '?' '!' Disjunction ')' | Atom Quantifier? )
       | ( PatternCharacter | '.' | '\' AtomEscape | CharacterClass | '(' Disjunction ')' | '(' '?' ':' Disjunction ')' )
           ( '*' | '+' | '?' | '{' DecimalDigits (',' DecimalDigits?)? '}' ) '?'?
PatternCharacter ::= <unicode character> but not any of { '^', '$', '\', '.', '*', '+', '?', '(', ')', '[', ']', '{', '}', '|' }
AtomEscape ::= DecimalEscape | CharacterEscape | CharacterClassEscape
CharacterEscape ::= ControlEscape | 'c' ControlLetter | HexEscapeSequence | UnicodeEscapeSequence | IdentityEscape
ControlEscape ::= one of { 'f', 'n', 'r', 't', 'v' }
ControlLetter ::= one of { 'a'..'z', 'A'..'Z' }
IdentityEscape ::= <unicode character> but not <unicode Uppercase letter>, <unicode Lowercase letter>, <unicode Titlecase letter>, <unicode Modifier letter>, <unicode Other letter>, <unicode Letter number>, '$', '_', <unicode Non-spacing mark>, <unicode combining spacing mark>, <unicode Decimal number>, <unicode Connector punctuation>
DecimalEscape ::= DecimalIntegerLiteral [lookahead not in DecimalDigit]
CharacterClassEscape :: one of { 'd', 'D', 's', 'S', 'w', 'W' }
CharacterClass ::= '[' [lookahead not in {'^'}] ClassRanges ']' | '[' '^' ClassRanges ']'
ClassRanges ::= [empty] | NonemptyClassRanges
NonemptyClassRanges ::= ClassAtom | ClassAtom NonemptyClassRangesNoDash | ClassAtom '-' ClassAtom ClassRanges
NonemptyClassRangesNoDash ::= ClassAtom | ClassAtomNoDash NonemptyClassRangesNoDash | ClassAtomNoDash '-' ClassAtom ClassRanges
ClassAtom ::= '-' | ClassAtomNoDash
ClassAtomNoDash ::= SourceCharacter but not one of { '\', ']', '-' } | '\' ClassEscape
ClassEscape ::= DecimalEscape | 'b' | CharacterEscape | CharacterClassEscape
HexEscapeSequence ::= 'x' HexDigit{2}
UnicodeEscapeSequence ::= 'u' HexDigit{4} | 'u' '{' HexDigits{4-6} '}'
HexDigit ::= one of { '0'..'9', 'a'..'f', 'A'..'F' }
DecimalIntegerLiteral ::= '0' | NonZeroDigit DecimalDigits?
DecimalDigits ::= DecimalDigit+
DecimalDigit ::= one of { '0'..'9' }
NonZeroDigit ::= one of { '1'..'9' }

------------------
Annex B Deviations
------------------

QuantifiableAssertion [added] ::= '(' '?' '=' Disjunction ')' | '(' '?' '!' Disjunction ')'
Term ::= ... | '(' '?' '=' Disjunction ')' [removed] | '(' '?' '!' Disjunction ')' [removed] | QuantifiableAssertion Quantifier? [added]

*/

#include "ParserPch.h"
#include "RegexUnicodeProperty.h"

namespace UnifiedRegex
{
    // Thrown when the speculative pre-flag pass-0 v-class validation reaches
    // the end of the regex literal. The speculative scan must never hard-fail
    // a pattern that is valid under the legacy (non-v) grammar; the same text
    // is re-parsed in pass 1 once the flags are known.
    struct VScanAbort {};

    template <typename P, const bool IsLiteral>
    const CharCount Parser<P, IsLiteral>::initLitbufSize;

    namespace VDetail
    {
        inline bool IsReservedPunctuatorChar(char16 c)
        {
            switch (c)
            {
                case '&': case '!': case '#': case '$': case '%': case '*':
                case '+': case ',': case '.': case ':': case ';': case '<':
                case '=': case '>': case '?': case '@': case '`': case '~':
                case '^':
                    return true;
                default:
                    return false;
            }
        }

        inline bool IsHighSurrogateUnit(char16 c) { return c >= 0xD800 && c <= 0xDBFF; }
        inline bool IsLowSurrogateUnit(char16 c) { return c >= 0xDC00 && c <= 0xDFFF; }
        inline codepoint_t SurrogatePairToCodePoint(char16 high, char16 low)
        {
            return 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
        }
        inline bool IsClassSetSyntaxChar(char16 c)
        {
            switch (c)
            {
                case '(': case ')': case '[': case ']': case '{': case '}':
                case '-': case '/': case '|':
                    return true;
                default:
                    return false;
            }
        }
    }


    ParseError::ParseError(bool isBody, CharCount pos, CharCount encodedPos, HRESULT error)
        : isBody(isBody), pos(pos), encodedPos(encodedPos), error(error)
    {
    }

    template <typename P, const bool IsLiteral>
    Parser<P, IsLiteral>::Parser
        ( Js::ScriptContext* scriptContext
        , ArenaAllocator* ctAllocator
        , StandardChars<EncodedChar>* standardEncodedChars
        , StandardChars<Char>* standardChars
        , bool isUtf8
#if ENABLE_REGEX_CONFIG_OPTIONS
        , DebugWriter* w
#endif
        )
        : scriptContext(scriptContext)
        , ctAllocator(ctAllocator)
        , standardEncodedChars(standardEncodedChars)
        , standardChars(standardChars)
#if ENABLE_REGEX_CONFIG_OPTIONS
        , w(w)
#endif
        , input(0)
        , inputLim(0)
        , next(0)
        , inBody(false)
        , numGroups(1)
        , nextGroupId(1) // implicit overall group always takes index 0
        , litbuf(0)
        , litbufLen(0)
        , litbufNext(0)
        , namedGroups(nullptr)
        , numNamedGroups(0)
        , namedGroupsCap(0)
        , namedGroupRefSpans(nullptr)
        , numNamedGroupRefSpans(0)
        , namedGroupRefSpansCap(0)
        , namedGroupBuffer(nullptr)
        , namedGroupBufferLen(0)
        , namedGroupBufferNext(0)
        , namedGroupBufferCap(0)
        , sawBareK(false)
        , sawClassK(false)
        , altPathGroupIdx(nullptr)
        , altPathCount(0)
        , altPathCap(0)
        , altScopes(nullptr)
        , altScopeCount(0)
        , altScopeCap(0)
        , altFrameStart(0)
        , lookbehindDepth(0)
        , surrogatePairList(nullptr)
        , currentSurrogatePairNode(nullptr)
        , tempLocationOfSurrogatePair(nullptr)
        , tempLocationOfRange(nullptr)
        , codePointAtTempLocation(0)
        , unicodeFlagPresent(false)
        , dotAllFlagPresent(false)
        , caseInsensitiveFlagPresent(false)
        , multilineFlagPresent(false)
        , hasScopedModifiers(false)
        , positionAfterLastSurrogate(nullptr)
        , valueOfLastSurrogate(INVALID_CODEPOINT)
        , deferredIfNotUnicodeError(nullptr)
        , deferredIfUnicodeError(nullptr)
        , deferredIfUnicodeSetsError(nullptr)
        , deferredIfUnicodeNoSetsError(nullptr)
        , unicodeSetsFlagPresent(false)
        , vStringBuffer(nullptr)
        , vStringBufLen(0)
        , vStringBufNext(0)
        , vStringBufCap(0)
        , vScratchCps(nullptr)
        , vScratchCpsCap(0)
        , vScratchUnits(nullptr)
        , vScratchUnitsLen(0)
        , vScratchUnitsCap(0)
        , vLastScanIsStringProperty(false)
        , vClassSawNested(false)
        , vScanSpeculative(false)
        , vScanLimit(nullptr)
        , vClassSpanStart(nullptr)
        , vClassSpanEnd(nullptr)
        , decimalEscapes(nullptr)
        , numDecimalEscapes(0)
        , decimalEscapesCap(0)
    {
        this->SetIsUtf8(isUtf8);
    }

    //
    // Input buffer management
    //

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::SetPosition(const EncodedChar* input, const EncodedChar* inputLim, bool inBody)
    {
        this->input = input;
        this->inputLim = inputLim;
        next = input;
        this->inBody = inBody;
        this->RestoreMultiUnits(0);
    }

    template <typename P, const bool IsLiteral>
    inline CharCount Parser<P, IsLiteral>::Pos()
    {
        CharCount nextOffset = Chars<EncodedChar>::OSB(next, input);
        Assert(nextOffset >= this->m_cMultiUnits);
        return nextOffset - (CharCount) this->m_cMultiUnits;
    }

    template <typename P, const bool IsLiteral>
    inline bool Parser<P, IsLiteral>::IsEOF()
    {
        return next >= inputLim;
    }

    template <typename P, const bool IsLiteral>
    inline bool Parser<P, IsLiteral>::ECCanConsume(CharCount n /*= 1*/)
    {
        return next + n <= inputLim;
    }

    template <typename P, const bool IsLiteral>
    inline typename P::EncodedChar Parser<P, IsLiteral>::ECLookahead(CharCount n /*= 0*/)
    {
        // Ok to look ahead to terminating 0
        Assert(next + n <= inputLim);
        return next[n];
    }

    template <typename P, const bool IsLiteral>
    inline typename P::EncodedChar Parser<P, IsLiteral>::ECLookback(CharCount n /*= 0*/)
    {
        // Ok to look ahead to terminating 0
        Assert(n + input <= next);
        return *(next - n);
    }

    template <typename P, const bool IsLiteral>
    inline void Parser<P, IsLiteral>::ECConsume(CharCount n /*= 1*/)
    {
        Assert(next + n <= inputLim);
#if DBG
        for (CharCount i = 0; i < n; i++)
            Assert(!this->IsMultiUnitChar(next[i]));
#endif
        next += n;
    }

    template <typename P, const bool IsLiteral>
    inline void Parser<P, IsLiteral>::ECConsumeMultiUnit(CharCount n /*= 1*/)
    {
        Assert(next + n <= inputLim);
        next += n;
    }

    template <typename P, const bool IsLiteral>
    inline void Parser<P, IsLiteral>::ECRevert(CharCount n /*= 1*/)
    {
        Assert(n + input <= next);
        next -= n;
    }

    //
    // Helpers
    //
    template <typename P, const bool IsLiteral>
    int Parser<P, IsLiteral>::TryParseExtendedUnicodeEscape(Char& c, bool& previousSurrogatePart, bool trackSurrogatePair /* = false */)
    {
        if (!scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
        {
            return 0;
        }

        if (!ECCanConsume(2) || ECLookahead(0) !='{' || !standardEncodedChars->IsHex(ECLookahead(1)))
        {
            return 0;
        }

        // The first character is mandatory to consume escape sequence, so we check for it above, at this stage we can set it as we already checked.
        codepoint_t codePoint = standardEncodedChars->DigitValue(ECLookahead(1));

        int i = 2;

        while(ECCanConsume(i + 1) && standardEncodedChars->IsHex(ECLookahead(i)))
        {
            codePoint <<= 4;
            codePoint += standardEncodedChars->DigitValue(ECLookahead(i));

            if (codePoint > 0x10FFFF)
            {
                DeferredFailIfUnicode(JSERR_RegExpInvalidEscape);
                return 0;
            }
            i++;
        }

        if(!ECCanConsume(i + 1) || ECLookahead(i) != '}')
        {
            // \u{ started a code point body (at least one hex digit) that
            // does not terminate with '}': no other RegExpUnicodeEscapeSequence
            // form can follow, so u mode must reject the pattern (deferred;
            // non-u keeps the identity-escape-plus-literals Annex B reading).
            DeferredFailIfUnicode(JSERR_RegExpInvalidEscape);
            return 0;
        }

        uint consumptionNumber = i + 1;
        Assert(consumptionNumber >= 3);

        if (!previousSurrogatePart && trackSurrogatePair && this->scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
        {
            // Current location
            TrackIfSurrogatePair(codePoint, (next - 1), consumptionNumber + 1);
        }

        char16 other;
        // Generally if this code point is a single character, then we take it and return.
        // If the character is made up of two characters then we emit the first and backtrack to the start of th escape sequence;
        // Following that we check if we have already seen the first character, and if so emit the second and consume the entire escape sequence.
        if (codePoint < 0x10000)
        {
            c = UTC(codePoint);
            ECConsumeMultiUnit(consumptionNumber);
        }
        else if (previousSurrogatePart)
        {
            previousSurrogatePart = false;
            Js::NumberUtilities::CodePointAsSurrogatePair(codePoint, &other, &c);
            ECConsumeMultiUnit(consumptionNumber);
        }
        else
        {
            previousSurrogatePart = true;
            Js::NumberUtilities::CodePointAsSurrogatePair(codePoint, &c, &other);
            Assert(ECLookback(1) == 'u' && ECLookback(2) == '\\');
            ECRevert(2);
        }

        return consumptionNumber;
    }

    // This function has the following 'knowledge':
    //     - A codepoint that might be part of a surrogate pair or part of one
    //     - A location where that codepoint is located
    //     - Previously tracked part of surrogate pair (tempLocationOfSurrogatePair, and codePointAtTempLocation), which is always a surrogate lower part.
    //     - A pointer to the current location of the linked list
    //     - If a previous location is tracked, then it is of a parsed character (not given character) before current, and not same to current
    //       This can't be asserted directly, and has to be followed by callers. Term pass can reset with each iteration, as well as this method in cases it needs.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>:: TrackIfSurrogatePair(codepoint_t codePoint,  const EncodedChar* location, uint32 consumptionLength)
    {
        Assert(codePoint < 0x110000);
        Assert(location != nullptr);
        Assert(location != this->tempLocationOfSurrogatePair);

        if (Js::NumberUtilities::IsSurrogateLowerPart(codePoint))
        {
            this->tempLocationOfSurrogatePair = location;
            this->codePointAtTempLocation = codePoint;
        }
        else
        {
            if(Js::NumberUtilities::IsSurrogateUpperPart(codePoint) && this->tempLocationOfSurrogatePair != nullptr)
            {
                Assert(Js::NumberUtilities::IsSurrogateLowerPart(codePointAtTempLocation));
                consumptionLength = (uint32)(location - this->tempLocationOfSurrogatePair) + consumptionLength;
                codePoint = Js::NumberUtilities::SurrogatePairAsCodePoint(codePointAtTempLocation, codePoint);
                location = this->tempLocationOfSurrogatePair;
            }
            // At this point we can clear previous location, and then if codePoint is bigger than 0xFFFF store it, as we either received it or combined it above
            this->tempLocationOfSurrogatePair = nullptr;
            this->codePointAtTempLocation = 0;
        }

        if (codePoint > 0xFFFF)
        {
            this->positionAfterLastSurrogate = location + consumptionLength;
            this->valueOfLastSurrogate = codePoint;

            // When parsing without AST we aren't given an allocator. In addition, only the 2 lines above are used during Pass 0;
            // while the bottom is used during Pass 1 (which isn't done when ParseNoAST)
            if(this->ctAllocator != nullptr)
            {
                SurrogatePairTracker* node = Anew(this->ctAllocator, SurrogatePairTracker, location, this->tempLocationOfRange, codePoint, consumptionLength, this->m_cMultiUnits);
                if (surrogatePairList == nullptr)
                {
                    Assert(currentSurrogatePairNode == nullptr);
                    surrogatePairList = node;
                    currentSurrogatePairNode = node;
                }
                else
                {
                    Assert(currentSurrogatePairNode != nullptr);
                    currentSurrogatePairNode->next = node;
                    currentSurrogatePairNode = node;
                }
            }
        }
    }
    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::CreateSurrogatePairAtom(char16 lower, char16 upper)
    {
        MatchLiteralNode * literalNode = Anew(this->ctAllocator, MatchLiteralNode, 0, 0, caseInsensitiveFlagPresent);
        MatchCharNode lowerNode(lower, caseInsensitiveFlagPresent);
        MatchCharNode upperNode(upper, caseInsensitiveFlagPresent);
        AccumLiteral(literalNode, &lowerNode);
        AccumLiteral(literalNode, &upperNode);

        return literalNode;
    }


    //
    // Unicode property escapes (ES2018, \p{...}/\P{...} under the u flag)
    //

    template <typename P, const bool IsLiteral>
    bool Parser<P, IsLiteral>::ScanPropertyEscapeBody(char16* name, CharCount& nameLen,
                                                      char16* value, CharCount& valueLen, bool& hasValue)
    {
        Assert(ECLookahead() == '{');

        nameLen = 0;
        valueLen = 0;
        hasValue = false;
        bool valid = true; // terminated body with plausible name shapes

        ECConsume(); // {
        while (true)
        {
            if (IsEOF())
            {
                // Unterminated body; the caller decides whether this is a
                // deferred-if-unicode error (pass 0) or a hard failure
                // (pass 1). The cursor is left wherever the scan stopped.
                return false;
            }
            EncodedChar ec = ECLookahead();
            if (ec == '}')
            {
                ECConsume();
                return valid && nameLen > 0 && (!hasValue || valueLen > 0);
            }
            if (ec == '/' && IsLiteral)
            {
                // An unescaped '/' terminates a regex literal body, so the
                // property body is unterminated from the parser's viewpoint.
                return false;
            }
            Char c = NextChar();
            if (c == '=')
            {
                if (hasValue)
                    valid = false; // at most one '=' separator
                hasValue = true;
                continue;
            }
            if (c >= 0x80)
            {
                valid = false; // non-ASCII names are always invalid
                continue;
            }
            char16* target = hasValue ? value : name;
            CharCount& targetLen = hasValue ? valueLen : nameLen;
            if (targetLen < propertyEscapeMaxNameLen)
                target[targetLen++] = c;
            else
                valid = false; // longer than any valid spelling
        }
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::ScanPropertyEscapePass0()
    {
        Assert(ECLookahead() == 'p' || ECLookahead() == 'P');

        if (ECLookahead(1) != '{')
        {
            // \p/\P without a body: identity escape in non-unicode mode,
            // invalid escape in unicode mode.
            DeferredFailIfUnicode(JSERR_RegExpInvalidEscape);
            ECConsume(); // p/P
            return;
        }

        ECConsume(); // p/P
        ScanPropertyEscapeBodyPass0(/*isNegation*/ ECLookback(1) == 'P');
    }

    // Shared pass 0 tail for the atom context (which consumed p/P through
    // ScanPropertyEscapePass0) and the class context (whose ClassEscapePass0
    // switch already consumed p/P through NextChar). Cursor is at '{'.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::ScanPropertyEscapeBodyPass0(bool isNegation)
    {
        Assert(ECLookahead() == '{');

        // A validation failure reports the escape's start, and an
        // unterminated body must leave the cursor just past 'p'/'P' so the
        // non-unicode re-parse as identity escape plus literals sees the
        // same input in both passes. 'p'/'P' is one encoded unit, so
        // next - 1 is the escape start.
        const EncodedChar* escapeStart = next - 1;
        const size_t savedMultiUnits = this->GetMultiUnits();

        char16 name[propertyEscapeMaxNameLen];
        char16 value[propertyEscapeMaxNameLen];
        CharCount nameLen, valueLen;
        bool hasValue;
        this->vLastScanIsStringProperty = false;
        bool structural = ScanPropertyEscapeBody(name, nameLen, value, valueLen, hasValue);
        if (!structural)
        {
            // Unterminated \p{ : SyntaxError only under the u flag.
            next = escapeStart + 1;
            this->RestoreMultiUnits(savedMultiUnits);
            DeferredFailIfUnicode(JSERR_RegExpSyntax);
            return;
        }
        UnicodeProperty::Resolved resolved;
        if (!UnicodeProperty::Resolve(name, nameLen, value, valueLen, hasValue, &resolved))
        {
            // Unknown property/value: SyntaxError only under the u flag;
            // without it the escape re-parses as the identity escape
            // 'p'/'P' plus literal '{...}' characters.
            const EncodedChar* savedNext = next;
            next = escapeStart;
            DeferredFailIfUnicode(JSERR_RegExpSyntax);
            next = savedNext;
            return;
        }
        // Properties of strings are valid only under the v flag: \P{...} of
        // one is a SyntaxError under every unicode mode (the complement of a
        // set that may contain strings is not expressible), while \p{...} of
        // one is a SyntaxError under plain u. Both fire after the flags are
        // known; under v the \P form is additionally the v-class rule.
        if (UnicodeProperty::IsStringProperty(resolved))
        {
            const EncodedChar* savedNext = next;
            next = escapeStart;
            if (isNegation)
                DeferredFailIfUnicode(JSERR_RegExpSyntax);
            else
                DeferredFailIfUnicodeNoSets(JSERR_RegExpSyntax);
            next = savedNext;
            this->vLastScanIsStringProperty = true;
        }
    }

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::ParsePropertyEscapePass1()
    {
        Assert(unicodeFlagPresent && (ECLookahead() == 'p' || ECLookahead() == 'P'));

        const bool isNegation = ECLookahead() == 'P';
        ECConsume(); // p/P
        if (IsEOF() || ECLookahead() != '{')
            Fail(JSERR_RegExpInvalidEscape); // pass 0 already deferred this

        char16 name[propertyEscapeMaxNameLen];
        char16 value[propertyEscapeMaxNameLen];
        CharCount nameLen, valueLen;
        bool hasValue;
        if (!ScanPropertyEscapeBody(name, nameLen, value, valueLen, hasValue))
            Fail(JSERR_RegExpSyntax);
        UnicodeProperty::Resolved resolved;
        if (!UnicodeProperty::Resolve(name, nameLen, value, valueLen, hasValue, &resolved))
            Fail(JSERR_RegExpSyntax);

        // Property of strings (v mode only): an alternation of the string
        // members (longest first) and the code point part, built exactly like
        // a v-mode class operand. \P is rejected (pass 0 deferred it).
        if (UnicodeProperty::IsStringProperty(resolved))
        {
            if (isNegation)
                Fail(JSERR_RegExpSyntax);

            VCSet* set = Anew(ctAllocator, VCSet);
            VInitSet(*set);
            struct Collector : UnicodeProperty::StringCollector
            {
                Parser* parser;
                VCSet* set;
                virtual void Add(const char16* s, int32 len) override
                {
                    CharCount cps = 0;
                    for (int32 i = 0; i < len; )
                    {
                        if (i + 1 < len && VDetail::IsHighSurrogateUnit(s[i]) && VDetail::IsLowSurrogateUnit(s[i + 1]))
                            i += 2;
                        else
                            i += 1;
                        cps++;
                    }
                    if (cps > parser->vScratchCpsCap)
                    {
                        CharCount newCap = parser->vScratchCpsCap == 0 ? 16 : parser->vScratchCpsCap;
                        while (cps > newCap)
                            newCap *= 2;
                        codepoint_t* newList = AnewArray(parser->ctAllocator, codepoint_t, newCap);
                        for (CharCount i = 0; i < cps; i++)
                            newList[i] = 0;
                        parser->vScratchCps = newList;
                        parser->vScratchCpsCap = newCap;
                    }
                    CharCount n = 0;
                    for (int32 i = 0; i < len; )
                    {
                        if (i + 1 < len && VDetail::IsHighSurrogateUnit(s[i]) && VDetail::IsLowSurrogateUnit(s[i + 1]))
                        {
                            parser->vScratchCps[n++] = VDetail::SurrogatePairToCodePoint(s[i], s[i + 1]);
                            i += 2;
                        }
                        else
                        {
                            parser->vScratchCps[n++] = s[i];
                            i += 1;
                        }
                    }
                    parser->VAddStringFromCodePoints(*set, parser->vScratchCps, n);
                }
            } collector;
            collector.parser = this;
            collector.set = set;
            if (!UnicodeProperty::BuildStringPropertySet(resolved, ctAllocator, set->cp, &collector))
                Fail(JSERR_RegExpSyntax); // ICU failed
            return VBuildNode(*set, /*negation*/ false);
        }

        CharSet<codepoint_t> codePointSet;
        if (!UnicodeProperty::BuildCodePointSet(resolved, ctAllocator, codePointSet))
            Fail(JSERR_RegExpSyntax); // ICU failed to compute the set

        // \P{...} complements the property over all code points first, and
        // i-mode case closure (when effective) applies to the complemented
        // set, so /\P{Lu}/iu matches 'A' through 'a' (Node-verified), while
        // [^\p{Lu}] closes over \p{Lu} and then complements in the class
        // path below.
        CharSet<codepoint_t> secondSet; // complement (\P) or case closure (i)
        CharSet<codepoint_t> thirdSet;  // case closure when both applied
        CharSet<codepoint_t>* toUse = &codePointSet;
        if (isNegation)
        {
            codePointSet.ToComplement(ctAllocator, secondSet);
            codePointSet.FreeBody(ctAllocator);
            toUse = &secondSet;
        }
        if (caseInsensitiveFlagPresent) // effective i state (scoped modifiers)
        {
            toUse->ToEquivClass(ctAllocator, thirdSet);
            toUse->FreeBody(ctAllocator);
            toUse = &thirdSet;
        }

        return TranslateCodePointSet(*toUse, /*isNegation*/ false);
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::UnionPropertyEscapePass1(CharSet<codepoint_t>& targetSet)
    {
        Assert(unicodeFlagPresent && (ECLookahead() == 'p' || ECLookahead() == 'P'));

        const bool isNegation = ECLookahead() == 'P';
        ECConsume(); // p/P
        if (IsEOF() || ECLookahead() != '{')
            Fail(JSERR_RegExpInvalidEscape); // pass 0 already deferred this shape

        char16 name[propertyEscapeMaxNameLen];
        char16 value[propertyEscapeMaxNameLen];
        CharCount nameLen, valueLen;
        bool hasValue;
        if (!ScanPropertyEscapeBody(name, nameLen, value, valueLen, hasValue))
            Fail(JSERR_RegExpSyntax);
        UnicodeProperty::Resolved resolved;
        if (!UnicodeProperty::Resolve(name, nameLen, value, valueLen, hasValue, &resolved))
            Fail(JSERR_RegExpSyntax);

        CharSet<codepoint_t> codePointSet;
        if (!UnicodeProperty::BuildCodePointSet(resolved, ctAllocator, codePointSet))
            Fail(JSERR_RegExpSyntax); // ICU failed to compute the set

        if (!isNegation)
        {
            targetSet.UnionInPlace(ctAllocator, codePointSet);
            codePointSet.FreeBody(ctAllocator);
        }
        else
        {
            CharSet<codepoint_t> complementedSet;
            codePointSet.ToComplement(ctAllocator, complementedSet);
            codePointSet.FreeBody(ctAllocator);
            targetSet.UnionInPlace(ctAllocator, complementedSet);
            complementedSet.FreeBody(ctAllocator);
        }
    }

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::TranslateCodePointSet(CharSet<codepoint_t>& codePointSet, bool isNegation,
        bool noHalfPairFallback)
    {
        Assert(unicodeFlagPresent);
        Assert(scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled());

        if (codePointSet.IsEmpty())
        {
            return Anew(ctAllocator, MatchSetNode, false, false, caseInsensitiveFlagPresent);
        }

        // If a singleton, return a simple character (or surrogate pair atom)
        bool isSingleton = !this->caseInsensitiveFlagPresent && !isNegation && codePointSet.IsSingleton();
        if (isSingleton)
        {
            codepoint_t singleton = codePointSet.Singleton();

            if (singleton < 0x10000)
            {
                Node* toReturn = Anew(ctAllocator, MatchCharNode, (char16)singleton, caseInsensitiveFlagPresent);
                codePointSet.Clear(ctAllocator);
                return toReturn;
            }
            else
            {
                char16 lowerSurrogate, upperSurrogate;
                Js::NumberUtilities::CodePointAsSurrogatePair(singleton, &lowerSurrogate, &upperSurrogate);
                Node* toReturn = CreateSurrogatePairAtom(lowerSurrogate, upperSurrogate);
                codePointSet.Clear(ctAllocator);
                return toReturn;
            }
        }

        const uint totalCodePointsCount = codePointSet.Count();
        const uint simpleCharsCount = codePointSet.SimpleCharCount();
        if (totalCodePointsCount == simpleCharsCount)
        {
            // Every u-mode caller of this helper hands in an
            // already-complemented set when isNegation is set
            // (CharacterClassPass1 pre-complements; the property-escape and
            // v-class callers pass false), so the node must not negate
            // again at runtime. Re-negating here double-negated the
            // all-BMP corner (the S3-a-documented
            // [^\u{10000}-\u{10FFFF}] shape, and [^\W_] once the
            // complement shorthands carry the astral planes).
            MatchSetNode *simpleToReturn = Anew(ctAllocator, MatchSetNode, false, true, caseInsensitiveFlagPresent);
            codePointSet.CloneSimpleCharsTo(ctAllocator, simpleToReturn->set);
            return simpleToReturn;
        }

        // Mixed BMP/astral membership: ONE deterministic whole-code-point-set
        // atom. The previous representation (BMP prefix + surrogate-pair
        // range arms + lone-surrogate suffix as an alternation) gave a
        // quantified atom several live arms per iteration — a surrogate pair
        // could be consumed as one pair arm or as lone units — so a failing
        // continuation enumerated the partitions and backtracked
        // exponentially. The single matcher resolves each position to
        // exactly one interpretation (member pair = 2 units, else unit).
        MatchCodePointSetNode* node = Anew(ctAllocator, MatchCodePointSetNode, caseInsensitiveFlagPresent);
        node->noHalfPairFallback = noHalfPairFallback;
        codePointSet.CloneSimpleCharsTo(ctAllocator, node->set);

        codepoint_t charRangeSearchIndex = 0x10000, lowerCharOfRange = 0, upperCharOfRange = 0;
        int numRanges = 0;
        while (codePointSet.GetNextRange(charRangeSearchIndex, &lowerCharOfRange, &upperCharOfRange))
        {
            numRanges++;
            charRangeSearchIndex = upperCharOfRange + 1;
        }
        node->numRanges = numRanges;
        node->ranges = AnewArray(ctAllocator, codepoint_t, 2 * numRanges);
        charRangeSearchIndex = 0x10000;
        int rangeIndex = 0;
        while (codePointSet.GetNextRange(charRangeSearchIndex, &lowerCharOfRange, &upperCharOfRange))
        {
            node->ranges[2 * rangeIndex] = lowerCharOfRange;
            node->ranges[2 * rangeIndex + 1] = upperCharOfRange;
            rangeIndex++;
            charRangeSearchIndex = upperCharOfRange + 1;
        }

        node->firstUnits.CloneFrom(ctAllocator, node->set);
        if (numRanges > 0)
        {
            // Any member pair starts with a high surrogate; including them
            // all keeps firstSet a sound upper bound (isFirstExact stays
            // false for the annotation passes).
            node->firstUnits.SetRange(ctAllocator, (Char)0xD800, (Char)0xDBFF);
        }

        codePointSet.Clear(ctAllocator);
        return node;
    }

    //
    // v flag (ES2024 Unicode sets) character classes.
    //
    // Grammar (validated in pass 0 with deferred-if-v errors, parsed and
    // compiled in pass 1 when the v flag is known), per the ES2024 spec and
    // the Node 26.5.0/Bun 1.4.3 oracles:
    //
    //   VClass     ::= '[' '^'? ( VExpr | empty ) ']'
    //   VExpr      ::= firstItem then a sequence of:
    //                  - juxtaposed items (union), or
    //                  - ('--' | '&&') operand chains (one operator kind
    //                    only, left-associative; ranges and multi-item
    //                    unions cannot be chain operands, and no union item
    //                    may follow a chain).
    //   VItem      ::= VOperand | singleton '-' singleton   (range)
    //   VOperand   ::= raw char | singleton escape | '\d'&co | '\p{'... |
    //                  '\q{'... | nested VClass
    //
    // Plus the reserved-double-punctuator rule (two adjacent unescaped
    // occurrences of any of & ! # $ % * + , . : ; < = > ? @ ` ~ ^, including
    // inside \q bodies and nested classes, unless the pair is the '&&'/'--'
    // operator itself), the ClassSetSyntaxCharacter escaping rule
    // ( ( ) [ ] { } - / | must be escaped inside classes and \q bodies), and
    // the negation early error (a '^' class whose contents MayContainStrings).
    //

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VInitSet(VCSet& set)
    {
        set.strings = nullptr;
        set.numStrings = 0;
        set.stringsCap = 0;
        set.containsEmptyString = false;
        set.mayContainStrings = false;
        set.isRange = false;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VEnsureStringCap(VCSet& set, int additional)
    {
        if (set.numStrings + additional <= set.stringsCap)
            return;
        int newCap = set.stringsCap == 0 ? 8 : set.stringsCap;
        while (set.numStrings + additional > newCap)
            newCap *= 2;
        VString* newList = AnewArray(ctAllocator, VString, newCap);
        for (int i = 0; i < set.numStrings; i++)
            newList[i] = set.strings[i];
        set.strings = newList;
        set.stringsCap = newCap;
    }

    // Appends chars to the shared string buffer, growing it (ctAllocator),
    // and returns the start offset of the appended run.
    template <typename P, const bool IsLiteral>
    CharCount Parser<P, IsLiteral>::VAppendStringChars(const Char* chars, CharCount len)
    {
        if (vStringBufNext + len > vStringBufCap)
        {
            CharCount newCap = vStringBufCap == 0 ? 32 : vStringBufCap;
            while (vStringBufNext + len > newCap)
                newCap *= 2;
            Char* newBuf = AnewArray(ctAllocator, Char, newCap);
            if (vStringBuffer != nullptr)
            {
                for (CharCount i = 0; i < vStringBufNext; i++)
                    newBuf[i] = vStringBuffer[i];
            }
            vStringBuffer = newBuf;
            vStringBufCap = newCap;
        }
        CharCount at = vStringBufNext;
        for (CharCount i = 0; i < len; i++)
            vStringBuffer[vStringBufNext + i] = chars[i];
        vStringBufNext += len;
        return at;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VAddStringRaw(VCSet& set, const Char* chars, CharCount unitLen, CharCount cpLen)
    {
        if (cpLen == 0)
        {
            set.containsEmptyString = true;
            set.mayContainStrings = true;
            return;
        }
        VEnsureStringCap(set, 1);
        CharCount at = VAppendStringChars(chars, unitLen);
        set.strings[set.numStrings].chars = vStringBuffer + at;
        set.strings[set.numStrings].unitLen = unitLen;
        set.strings[set.numStrings].cpLen = cpLen;
        set.numStrings++;
        set.mayContainStrings = true;
    }

    // Adds one string member given as code points, applying the
    // single-code-point folding (into the code point set) and, under the
    // effective i state, per-code-point canonicalization so the set
    // operations compare canonical forms (Node-verified behavior).
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VAddStringFromCodePoints(VCSet& set, const codepoint_t* cps, CharCount numCps)
    {
        if (numCps == 1)
        {
            // A single-code-point \q alternative participates in the code
            // point domain; under i its scf equivalence is exactly the
            // engine's case-fold closure of that code point (k ~ K ~
            // U+212A, s ~ S ~ U+017F), the same closure ordinary class
            // operands get.
            set.cp.Set(ctAllocator, cps[0]);
            if (caseInsensitiveFlagPresent)
            {
                VCloseOperandIfInsensitive(set);
            }
            return;
        }
        if (numCps == 0)
        {
            set.containsEmptyString = true;
            set.mayContainStrings = true;
            return;
        }
        // Canonicalize each code point, then encode to UTF-16 in a scratch
        // buffer (a canonicalized code point never changes unit count).
        CharCount needed = 0;
        for (CharCount i = 0; i < numCps; i++)
            needed += cps[i] >= 0x10000 ? 2 : 1;
        if (needed > vScratchUnitsCap)
        {
            CharCount newCap = vScratchUnitsCap == 0 ? 32 : vScratchUnitsCap;
            while (needed > newCap)
                newCap *= 2;
            Char* newBuf = AnewArray(ctAllocator, Char, newCap);
            if (vScratchUnits != nullptr)
            {
                for (CharCount i = 0; i < vScratchUnitsLen; i++)
                    newBuf[i] = vScratchUnits[i];
            }
            vScratchUnits = newBuf;
            vScratchUnitsCap = newCap;
        }
        CharCount units = 0;
        for (CharCount i = 0; i < numCps; i++)
        {
            codepoint_t cp = cps[i];
            if (caseInsensitiveFlagPresent)
                cp = VCanonicalizeCodePoint(cp);
            if (cp < 0x10000)
            {
                vScratchUnits[units++] = (Char)cp;
            }
            else
            {
                char16 first, second;
                Js::NumberUtilities::CodePointAsSurrogatePair(cp, &first, &second);
                vScratchUnits[units++] = first;
                vScratchUnits[units++] = second;
            }
        }
        vScratchUnitsLen = units;
        VAddStringRaw(set, vScratchUnits, units, numCps);
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VAddCodePoint(VCSet& set, codepoint_t cp)
    {
        if (caseInsensitiveFlagPresent)
            cp = VCanonicalizeCodePoint(cp);
        set.cp.Set(ctAllocator, cp);
    }

    // Folds an operand's code point part closed over the engine's case
    // equivalence classes (the same closure the legacy u-mode class path
    // applies) so that &&/-- see closed operands on both sides: under i,
    // [k--[k-k]] must be empty, not {K}. \q multi-code-point members are
    // not stored in the code point set (they are canonical literals); the
    // single-code-point members the \q path adds ARE closed here — their
    // scf equivalence is exactly this closure.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VCloseOperandIfInsensitive(VCSet& set)
    {
        if (!caseInsensitiveFlagPresent || set.cp.IsEmpty())
            return;
        CharSet<codepoint_t> closed;
        set.cp.ToEquivClass(ctAllocator, closed);
        set.cp.Clear(ctAllocator);
        set.cp.CloneFrom(ctAllocator, closed);
        closed.FreeBody(ctAllocator);
    }

    // Canonicalization for one code point under the effective i state: the
    // representative of its case-equivalence class, matching how the runtime
    // matches case-insensitive literals/backreferences under u/v programs.
    template <typename P, const bool IsLiteral>
    // Simple case folding for v string members under i (ES2027
    // MaybeSimpleCaseFolding): map every code point with Unicode simple
    // case folding (ICU u_foldCase, U_FOLD_CASE_DEFAULT). scf folds
    // U+212A (KELVIN SIGN) onto 'k' and U+017F (LATIN SMALL LETTER LONG S)
    // onto 's', so a \q member 'k'/'ss' matches those inputs, while ß
    // (whose simple folding is itself) does not match 'ss'. Node 26.5.0/V8
    // applies an asymmetric subset for single-character members (misses
    // U+212A there) — documented oracle divergence; this engine implements
    // the spec's symmetric scf(x) == scf(y) comparison.
    codepoint_t Parser<P, IsLiteral>::VCanonicalizeCodePoint(codepoint_t cp) const
    {
        return (codepoint_t)u_foldCase((UChar32)cp, U_FOLD_CASE_DEFAULT);
    }

    template <typename P, const bool IsLiteral>
    /*static*/ bool Parser<P, IsLiteral>::VStringEq(const VString& a, const VString& b)
    {
        if (a.unitLen != b.unitLen)
            return false;
        for (uint32 i = 0; i < a.unitLen; i++)
        {
            if (a.chars[i] != b.chars[i])
                return false;
        }
        return true;
    }

    template <typename P, const bool IsLiteral>
    /*static*/ int Parser<P, IsLiteral>::VStringLexCmp(const void* pa, const void* pb)
    {
        const VString& a = *static_cast<const VString*>(pa);
        const VString& b = *static_cast<const VString*>(pb);
        if (a.unitLen != b.unitLen)
            return a.unitLen < b.unitLen ? -1 : 1;
        for (uint32 i = 0; i < a.unitLen; i++)
        {
            if (a.chars[i] != b.chars[i])
                return a.chars[i] < b.chars[i] ? -1 : 1;
        }
        return 0;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VSortStringsLex(VString* strings, int n)
    {
        if (n > 1)
            qsort(strings, (size_t)n, sizeof(VString), &Parser<P, IsLiteral>::VStringLexCmp);
    }

    // acc OP rhs over both components: code points via range arithmetic,
    // strings via exact (already canonicalized) comparison. MayContainStrings
    // keeps the spec's conservative static semantics: a difference keeps the
    // left side's flag, an intersection is the conjunction of both.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VApplyOp(VCSet& acc, VCSet& rhs, bool isDifference)
    {
        // Code points
        CharSet<codepoint_t> out;
        if (isDifference)
        {
            codepoint_t aLo = 0, aHi = 0;
            codepoint_t search = 0;
            while (acc.cp.GetNextRange(search, &aLo, &aHi))
            {
                search = aHi + 1;
                // Subtract every rhs range overlapping [aLo, aHi]
                codepoint_t cursor = aLo;
                codepoint_t bLo = 0, bHi = 0;
                codepoint_t bSearch = 0;
                while (rhs.cp.GetNextRange(bSearch, &bLo, &bHi))
                {
                    bSearch = bHi + 1;
                    if (bHi < cursor)
                        continue;
                    if (bLo > aHi)
                        break;
                    if (bLo > cursor)
                        out.SetRange(ctAllocator, cursor, bLo - 1);
                    cursor = bHi < aHi ? bHi + 1 : aHi + 1;
                    if (cursor > aHi)
                        break;
                }
                if (cursor <= aHi)
                    out.SetRange(ctAllocator, cursor, aHi);
            }
        }
        else
        {
            codepoint_t aLo = 0, aHi = 0;
            codepoint_t search = 0;
            while (acc.cp.GetNextRange(search, &aLo, &aHi))
            {
                search = aHi + 1;
                codepoint_t bLo = 0, bHi = 0;
                codepoint_t bSearch = 0;
                while (rhs.cp.GetNextRange(bSearch, &bLo, &bHi))
                {
                    bSearch = bHi + 1;
                    if (bHi < aLo)
                        continue;
                    if (bLo > aHi)
                        break;
                    out.SetRange(ctAllocator, max(aLo, bLo), min(aHi, bHi));
                }
            }
        }
        acc.cp.Clear(ctAllocator);
        acc.cp.CloneFrom(ctAllocator, out);
        out.FreeBody(ctAllocator);

        // Strings
        if (isDifference)
        {
            if (rhs.numStrings > 0 && acc.numStrings > 0)
            {
                VSortStringsLex(acc.strings, acc.numStrings);
                VSortStringsLex(rhs.strings, rhs.numStrings);
                int outCount = 0;
                int j = 0;
                for (int i = 0; i < acc.numStrings; i++)
                {
                    while (j < rhs.numStrings && VStringLexCmp(&rhs.strings[j], &acc.strings[i]) < 0)
                        j++;
                    if (j < rhs.numStrings && VStringEq(rhs.strings[j], acc.strings[i]))
                        continue; // removed
                    acc.strings[outCount++] = acc.strings[i];
                }
                acc.numStrings = outCount;
            }
            if (rhs.containsEmptyString)
                acc.containsEmptyString = false;
            // mayContainStrings keeps the left side's flag (spec semantics)
        }
        else
        {
            if (acc.numStrings > 0 && rhs.numStrings > 0)
            {
                VSortStringsLex(acc.strings, acc.numStrings);
                VSortStringsLex(rhs.strings, rhs.numStrings);
                int outCount = 0;
                int j = 0;
                for (int i = 0; i < acc.numStrings; i++)
                {
                    while (j < rhs.numStrings && VStringLexCmp(&rhs.strings[j], &acc.strings[i]) < 0)
                        j++;
                    if (j < rhs.numStrings && VStringEq(rhs.strings[j], acc.strings[i]))
                        acc.strings[outCount++] = acc.strings[i];
                }
                acc.numStrings = outCount;
            }
            else
            {
                acc.numStrings = 0;
            }
            acc.containsEmptyString = acc.containsEmptyString && rhs.containsEmptyString;
            acc.mayContainStrings = acc.mayContainStrings && rhs.mayContainStrings;
        }
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VUnionInPlace(VCSet& acc, VCSet& rhs)
    {
        acc.cp.UnionInPlace(ctAllocator, rhs.cp);
        VEnsureStringCap(acc, rhs.numStrings);
        for (int i = 0; i < rhs.numStrings; i++)
            acc.strings[acc.numStrings++] = rhs.strings[i];
        acc.containsEmptyString = acc.containsEmptyString || rhs.containsEmptyString;
        acc.mayContainStrings = acc.mayContainStrings || rhs.mayContainStrings;
    }

    // ---------------------------------------------------------------------------
    // Pass 0 validation (deferred-if-v errors; cursor is fully restored)
    // ---------------------------------------------------------------------------

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VClassPass0()
    {
        const EncodedChar* savedNext = next;
        const size_t savedMultiUnits = this->GetMultiUnits();
        // Legacy class end (after its first unescaped ']'), computed without
        // touching parser state. The speculative v scan must stop there: past
        // it the text belongs to the rest of the literal, not this class.
        const EncodedChar* legacyEnd = next;
        while (legacyEnd < inputLim && *legacyEnd != 0)
        {
            if (*legacyEnd == '\\')
            {
                legacyEnd += (legacyEnd + 1 < inputLim) ? 2 : 1;
                continue;
            }
            if (*legacyEnd == ']')
            {
                ++legacyEnd;
                break;
            }
            ++legacyEnd;
        }
        const EncodedChar* const savedVScanLimit = this->vScanLimit;
        // The regex literal ends at the first unescaped '/' after the legacy
        // class close. A v class may legitimately extend past that close
        // (nested classes, e.g. /[a[b]c]/v), so do not stop at legacyEnd;
        // stop at the literal terminator. This lets valid nested v classes
        // finish while still preventing the scan from reading the tail of
        // the source line.
        const EncodedChar* literalEnd = legacyEnd;
        while (literalEnd < inputLim && *literalEnd != 0 &&
               *literalEnd != '/' && *literalEnd != '\n' && *literalEnd != '\r')
        {
            if (*literalEnd == '\\' && literalEnd + 1 < inputLim)
            {
                literalEnd += 2;
                continue;
            }
            ++literalEnd;
        }
        this->vScanLimit = literalEnd;
        bool mayContainStrings = false;
        this->vClassSawNested = false;
        // The speculative scan can leave the class and read trailing source
        // (up to a newline or '/'). Any deferred error it records from that
        // tail is not a property of this literal; snapshot the three
        // deferred slots and restore them if the scan aborts.
        ParseError* const savedUnicodeError = this->deferredIfUnicodeError;
        ParseError* const savedSetsError = this->deferredIfUnicodeSetsError;
        ParseError* const savedNoSetsError = this->deferredIfUnicodeNoSetsError;
        bool aborted = false;
        const bool savedVScanSpeculative = this->vScanSpeculative;
        this->vScanSpeculative = true;
        try
        {
            VValidateClass(mayContainStrings);
        }
        catch (VScanAbort&)
        {
            // Speculative scan hit the literal end. Flags are not known yet,
            // so this must not fail a legal legacy class; pass 1 re-parses
            // the text under the real flags.
            aborted = true;
            mayContainStrings = false;
            this->deferredIfUnicodeError = savedUnicodeError;
            this->deferredIfUnicodeSetsError = savedSetsError;
            this->deferredIfUnicodeNoSetsError = savedNoSetsError;
        }
        catch (...)
        {
            // Keep the speculative flag scoped even if a hard ParseError/OOM
            // escapes; the caller unwinds the parser, but a future caller
            // that catches and reuses it must not inherit a stale flag.
            this->vScanSpeculative = savedVScanSpeculative;
            this->vScanLimit = savedVScanLimit;
            throw;
        }
        this->vScanSpeculative = savedVScanSpeculative;
        this->vScanLimit = savedVScanLimit;
        // For the ']'-atom rule in TermPass0 (see there): record the span
        // the v validator consumed (from the '[' through its matching ']').
        // A legacy ']' atom is genuine v class content exactly when it lies
        // inside this span (nested brackets, \q bodies with '|' or ']');
        // outside it a bare ']' really is an atom and rejects under v.
        // A nested-in-legacy-view sibling ([[ab][cd]]: the legacy scan
        // starts a second class inside the v class's span) must not
        // shrink the recorded span; keep the enclosing one when the new
        // span lies inside it.
        if (!aborted)
        {
            const EncodedChar* newStart = savedNext - 1;
            const EncodedChar* newEnd = next;
            if (!(this->vClassSpanStart != nullptr &&
                  newStart >= this->vClassSpanStart && newEnd <= this->vClassSpanEnd))
            {
                this->vClassSpanStart = newStart;
                this->vClassSpanEnd = newEnd;
            }
        }
        next = savedNext;
        this->RestoreMultiUnits(savedMultiUnits);
    }

    // Cursor just after '[' (the '^' not yet consumed).
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VValidateClass(bool& mayContainStrings)
    {
        // Nested v classes recurse once per level; probe the stack so deep
        // nesting throws the engine's standard stack-overflow error instead
        // of exhausting the thread stack (Node accepts ~arbitrary depth,
        // V8 throws RangeError beyond its own limit; we stay in the
        // engine's standard probe framework like every other recursive
        // parser entry point).
        PROBE_STACK_NO_DISPOSE(scriptContext, Js::Constants::MinStackRegex);
        mayContainStrings = false;
        if (IsEOF() || ECLookahead() == 0)
        {
            DeferredFailIfUnicodeSets(JSERR_RegExpNoBracket);
            return;
        }
        bool negation = false;
        if (ECLookahead() == '^')
        {
            negation = true;
            ECConsume();
        }
        if (ECLookahead() == ']')
        {
            ECConsume(); // keep nested scans aligned (cursor is restored by the caller)
            return; // empty class
        }
        VValidateExpr(mayContainStrings);
        if (IsEOF() || ECLookahead() == 0 || ECLookahead() != ']')
        {
            DeferredFailIfUnicodeSets(JSERR_RegExpNoBracket);
            return;
        }
        ECConsume(); // the class's closing ']' (VParseClass consumes it too)
        if (negation && mayContainStrings)
        {
            // Negated character class may contain strings (ES2024 early
            // error; the static MayContainStrings is conservative).
            DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
        }
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VValidateExpr(bool& mayContainStrings)
    {
        mayContainStrings = false;
        int unionItems = 0;      // plain union items parsed before any operator
        bool sawRange = false;   // a range among them
        char chainOp = 0;

        // First item
        bool itemMcs = false;
        bool isRange = false;
        VValidateItem(itemMcs, isRange);
        mayContainStrings = itemMcs;
        sawRange = isRange;

        while (true)
        {
            if (IsEOF() || ECLookahead() == 0 || ECLookahead() == ']')
            {
                if (ECLookahead() != ']')
                    DeferredFailIfUnicodeSets(JSERR_RegExpNoBracket);
                return;
            }
            bool isAndOp = ECLookahead() == '&' && ECLookahead(1) == '&';
            bool isDiffOp = ECLookahead() == '-' && ECLookahead(1) == '-';
            if (isAndOp || isDiffOp)
            {
                if (unionItems != 0 || sawRange || (chainOp != 0 && chainOp != (isAndOp ? '&' : '-')))
                {
                    // A multi-item union or a range cannot be the left side
                    // of a set operation, and the two operators cannot mix:
                    // nest a class instead (V8/JSC-observed grammar).
                    DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
                    return;
                }
                chainOp = isAndOp ? '&' : '-';
                ECConsume(2);
                // A third operator char ([a&&&], [a---b]): the operator's
                // second char and it form a reserved double punctuator.
                if (ECLookahead() == chainOp && ECLookahead(1) != 0 && !IsEOF())
                {
                    DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
                    return;
                }
                bool rhsMcs = false;
                VValidateEscape(rhsMcs);
                // A range cannot be the right operand of a set operation:
                // '-' followed by an operand start. ('--' was consumed above
                // as the operator chain continuation; a trailing '-' before
                // ']' is reported by the next item scan.)
                if (ECLookahead() == '-' && ECLookahead(1) != ']' && ECLookahead(1) != '-' && ECLookahead(1) != 0)
                {
                    DeferredFailIfUnicodeSets(JSERR_UnicodeRegExpRangeContainsCharClass);
                    return;
                }
                if (chainOp == '&')
                    mayContainStrings = mayContainStrings && rhsMcs;
                // '-' keeps the left side's flag
                continue;
            }
            if (chainOp != 0)
            {
                // Juxtaposed item after a set-operation chain.
                DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
                return;
            }
            VValidateItem(itemMcs, isRange);
            mayContainStrings = mayContainStrings || itemMcs;
            sawRange = sawRange || isRange;
            unionItems++;
        }
    }

    // Validates one item (operand or range). Sets isRange.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VValidateItem(bool& mayContainStrings, bool& isRange)
    {
        isRange = false;
        bool isSetValued = false;
        bool isSingleton = false;
        VValidateEscape(mayContainStrings, isSingleton, isSetValued);
        // Range? '-' followed by an operand start (not ']', not the '--'
        // operator, not EOF).
        if (ECLookahead() == '-' && ECLookahead(1) != ']' && ECLookahead(1) != '-' && ECLookahead(1) != 0)
        {
            if (isSetValued)
            {
                DeferredFailIfUnicodeSets(JSERR_UnicodeRegExpRangeContainsCharClass);
                return;
            }
            ECConsume(); // -
            bool rhsMcs = false, rhsSingleton = false, rhsSetValued = false;
            VValidateEscape(rhsMcs, rhsSingleton, rhsSetValued);
            if (rhsSetValued)
            {
                DeferredFailIfUnicodeSets(JSERR_UnicodeRegExpRangeContainsCharClass);
                return;
            }
            isRange = true;
        }
    }

    // Validates one operand starting at the cursor: a raw character, a
    // nested class, or an escape. Reports the v-specific deferred errors
    // (reserved punctuator pairs, unescaped syntax characters, \q bodies)
    // and delegates property-name validation to the shared pass 0 property
    // scanner. Outputs: isSingleton (single code point), isSetValued
    // (\d&co, \p{...}, nested class, \q{...}).
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VValidateEscape(bool& mayContainStrings)
    {
        bool isSingleton = false;
        bool isSetValued = false;
        VValidateEscape(mayContainStrings, isSingleton, isSetValued);
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VValidateEscape(bool& mayContainStrings, bool& isSingleton, bool& isSetValued)
    {
        isSingleton = false;
        isSetValued = false;
        if (IsEOF() || ECLookahead() == 0)
        {
            DeferredFailIfUnicodeSets(JSERR_RegExpNoBracket);
            return;
        }
        EncodedChar ec = ECLookahead();
        if (ec == '\\')
        {
            ECConsume();
            if (IsEOF() || ECLookahead() == 0)
            {
                DeferredFailIfUnicodeSets(JSERR_RegExpNoBracket);
                return;
            }
            Char c = Chars<EncodedChar>::CTW(ECLookahead());
            switch (c)
            {
                case 'q':
                    ECConsume(); // q
                    if (ECLookahead() == '{')
                    {
                        VValidateQBody(mayContainStrings);
                        isSetValued = true;
                        return;
                    }
                    DeferredFailIfUnicodeSets(JSERR_RegExpInvalidEscape);
                    return;
                case 'p':
                case 'P':
                    ECConsume(); // p/P
                    if (ECLookahead() == '{')
                    {
                        ScanPropertyEscapeBodyPass0(/*isNegation*/ c == 'P');
                        mayContainStrings = mayContainStrings || this->vLastScanIsStringProperty;
                        isSetValued = true;
                        return;
                    }
                    DeferredFailIfUnicodeSets(JSERR_RegExpInvalidEscape);
                    return;
                case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
                    ECConsume();
                    isSetValued = true;
                    return;
                case 'f': case 'n': case 'r': case 't': case 'v': case 'b': case '0':
                    ECConsume();
                    isSingleton = true;
                    return;
                case 'c':
                    ECConsume();
                    if (!IsEOF() && ECLookahead() != 0 && standardEncodedChars->IsLetter(ECLookahead()))
                        ECConsume();
                    isSingleton = true;
                    return;
                case 'x':
                    ECConsume();
                    if (ECCanConsume(2) && standardEncodedChars->IsHex(ECLookahead(0)) && standardEncodedChars->IsHex(ECLookahead(1)))
                        ECConsume(2);
                    isSingleton = true;
                    return;
                case 'u':
                {
                    ECConsume(); // u
                    if (ECCanConsume(1) && ECLookahead(0) == '{')
                    {
                        // Scan the braced escape inline: the shared
                        // TryParseExtendedUnicodeEscape helper implements a
                        // two-call unit-splitting dance for astral code
                        // points that rewinds the cursor, which is wrong for
                        // this validation-only scan.
                        ECConsume(); // {
                        CharCount digits = 0;
                        while (!IsEOF() && ECLookahead() != 0 && standardEncodedChars->IsHex(ECLookahead()))
                        {
                            ECConsume();
                            digits++;
                        }
                        if (digits == 0 || IsEOF() || ECLookahead() != '}')
                        {
                            DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
                            return;
                        }
                        ECConsume(); // }
                    }
                    else if (ECCanConsume(4) &&
                        standardEncodedChars->IsHex(ECLookahead(0)) && standardEncodedChars->IsHex(ECLookahead(1)) &&
                        standardEncodedChars->IsHex(ECLookahead(2)) && standardEncodedChars->IsHex(ECLookahead(3)))
                    {
                        Char high = UTC((standardEncodedChars->DigitValue(ECLookahead(0)) << 12) |
                            (standardEncodedChars->DigitValue(ECLookahead(1)) << 8) |
                            (standardEncodedChars->DigitValue(ECLookahead(2)) << 4) |
                            standardEncodedChars->DigitValue(ECLookahead(3)));
                        ECConsume(4);
                        // An escaped surrogate pair is one code point
                        // (pass 1 combines it in VDecodeEscapeCodePoint).
                        if (VDetail::IsHighSurrogateUnit(high) &&
                            ECLookahead() == '\\' && ECCanConsume(6) && ECLookahead(1) == 'u' &&
                            standardEncodedChars->IsHex(ECLookahead(2)) && standardEncodedChars->IsHex(ECLookahead(3)) &&
                            standardEncodedChars->IsHex(ECLookahead(4)) && standardEncodedChars->IsHex(ECLookahead(5)))
                        {
                            Char low = UTC((standardEncodedChars->DigitValue(ECLookahead(2)) << 12) |
                                (standardEncodedChars->DigitValue(ECLookahead(3)) << 8) |
                                (standardEncodedChars->DigitValue(ECLookahead(4)) << 4) |
                                standardEncodedChars->DigitValue(ECLookahead(5)));
                            if (VDetail::IsLowSurrogateUnit(low))
                                ECConsume(6); // \uXXXX
                        }
                    }
                    isSingleton = true;
                    return;
                }
                default:
                    if (standardEncodedChars->IsDigit(ECLookahead()))
                    {
                        // Octal escapes (other than \0, handled above) are
                        // rejected under v. Consume first: a validation-only
                        // scan must never leave the cursor in place.
                        ECConsume();
                        DeferredFailIfUnicodeSets(JSERR_RegExpInvalidEscape);
                        return;
                    }
                    // Identity escape of a non-digit character. Under the v
                    // grammar an escaped class character is legal only as a
                    // CharacterEscape (handled by the cases above) or as
                    // escaped ClassSet punctuation: the ClassSetSyntax
                    // characters (including the backslash itself:
                    // [\\]/v is the escaped-backslash member), the
                    // reserved-double-punctuator components and the
                    // '-'/'^' operators. Everything else ([\" ], [\ ],
                    // [\'], [\k], non-ASCII, ...) rejects.
                    switch (c)
                    {
                    case '!':
                    case '#':
                    case '$':
                    case '%':
                    case '&':
                    case '\\':
                    case '(':
                    case ')':
                    case '*':
                    case '+':
                    case ',':
                    case '-':
                    case '.':
                    case '/':
                    case ':':
                    case ';':
                    case '<':
                    case '=':
                    case '>':
                    case '?':
                    case '@':
                    case '[':
                    case ']':
                    case '^':
                    case '`':
                    case '{':
                    case '|':
                    case '}':
                    case '~':
                        break;
                    default:
                        DeferredFailIfUnicodeSets(JSERR_RegExpInvalidEscape);
                        break;
                    }
                    ECConsume();
                    isSingleton = true;
                    return;
            }
        }
        if (ec == '[')
        {
            ECConsume();
            this->vClassSawNested = true;
            bool innerMcs = false;
            VValidateClass(innerMcs);
            mayContainStrings = mayContainStrings || innerMcs;
            isSetValued = true;
            return;
        }
        // Raw character. ']' here means an operand was expected (the
        // expression loops break on ']' before reaching this). All error
        // paths consume the offending character first: this is a
        // validation-only scan, and leaving the cursor in place would loop.
        // Read a full code point: a raw surrogate pair is ONE v-class
        // operand ([😀--x] would otherwise validate as two items and the
        // -- operator would be rejected as a union chain).
        // A raw character at or past the legacy class end belongs to the
        // rest of the literal, not this class. Stop without consuming; pass 1
        // re-parses under the real flags anyway.
        if (this->vScanSpeculative && next >= this->vScanLimit)
        {
            throw VScanAbort();
        }
        Char c = NextChar();
        if (VDetail::IsHighSurrogateUnit(c) && !IsEOF() && ECLookahead() != 0)
        {
            // Decode the next character through the encoding policy (a
            // raw UTF-8 astral char is several encoded units; peeking raw
            // bytes as UTF-16 units mis-detects pairs in literals).
            const EncodedChar* after = next;
            const size_t savedMultiUnits = this->GetMultiUnits();
            Char c2 = NextChar();
            if (!VDetail::IsLowSurrogateUnit(c2))
            {
                next = after; // not a pair; rewind the decoded char
                this->RestoreMultiUnits(savedMultiUnits);
            }
        }
        if (VDetail::IsClassSetSyntaxChar(c) && c != '[')
        {
            DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
            return;
        }
        if (VDetail::IsReservedPunctuatorChar(c) && !IsEOF() && ECLookahead() != 0)
        {
            const EncodedChar* after = next;
            const size_t savedMultiUnits = this->GetMultiUnits();
            Char c2 = NextChar();
            bool isPair = c2 == c;
            next = after;
            this->RestoreMultiUnits(savedMultiUnits);
            if (isPair)
            {
                DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
                return;
            }
        }
        isSingleton = true;
    }

    // Cursor at '{' of a \q{...} escape ('q' consumed).
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VValidateQBody(bool& mayContainStrings)
    {
        Assert(ECLookahead() == '{');
        ECConsume(); // {
        int altCodePoints = 0;
        while (true)
        {
            if (IsEOF() || ECLookahead() == 0)
            {
                DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
                return;
            }
            if (this->vScanSpeculative && next >= this->vScanLimit)
            {
                throw VScanAbort();
            }
            EncodedChar ec = ECLookahead();
            if (ec == '}')
            {
                ECConsume();
                if (altCodePoints != 1)
                    mayContainStrings = true;
                return;
            }
            if (ec == '|')
            {
                ECConsume();
                if (altCodePoints != 1)
                    mayContainStrings = true;
                altCodePoints = 0;
                continue;
            }
            if (ec == '\\')
            {
                ECConsume();
                if (IsEOF() || ECLookahead() == 0)
                {
                    DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
                    return;
                }
                Char c = Chars<EncodedChar>::CTW(ECLookahead());
                switch (c)
                {
                    case 'f': case 'n': case 'r': case 't': case 'v': case 'b': case '0':
                        ECConsume();
                        altCodePoints++;
                        continue;
                    case 'c':
                        ECConsume();
                        if (!IsEOF() && ECLookahead() != 0 && standardEncodedChars->IsLetter(ECLookahead()))
                        {
                            ECConsume();
                            altCodePoints++;
                            continue;
                        }
                        DeferredFailIfUnicodeSets(JSERR_RegExpInvalidEscape);
                        return;
                    case 'x':
                        ECConsume();
                        if (ECCanConsume(2) && standardEncodedChars->IsHex(ECLookahead(0)) && standardEncodedChars->IsHex(ECLookahead(1)))
                        {
                            ECConsume(2);
                            altCodePoints++;
                            continue;
                        }
                        DeferredFailIfUnicodeSets(JSERR_RegExpInvalidEscape);
                        return;
                    case 'u':
                    {
                        ECConsume();
                        if (ECCanConsume(1) && ECLookahead(0) == '{')
                        {
                            // Scan the braced escape inline (see the
                            // operand validator: the shared helper rewinds
                            // for astral code points).
                            ECConsume(); // {
                            CharCount digits = 0;
                            while (!IsEOF() && ECLookahead() != 0 && standardEncodedChars->IsHex(ECLookahead()))
                            {
                                ECConsume();
                                digits++;
                            }
                            if (digits == 0 || IsEOF() || ECLookahead() != '}')
                            {
                                DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
                                return;
                            }
                            ECConsume(); // }
                            altCodePoints++;
                            continue;
                        }
                        if (ECCanConsume(4) &&
                            standardEncodedChars->IsHex(ECLookahead(0)) && standardEncodedChars->IsHex(ECLookahead(1)) &&
                            standardEncodedChars->IsHex(ECLookahead(2)) && standardEncodedChars->IsHex(ECLookahead(3)))
                        {
                            ECConsume(4);
                            altCodePoints++;
                            continue;
                        }
                        ECConsume(); // malformed; keep the scan advancing
                        DeferredFailIfUnicodeSets(JSERR_RegExpInvalidEscape);
                        return;
                    }
                    case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
                    case 'p': case 'P': case 'k': case 'q':
                        ECConsume();
                        DeferredFailIfUnicodeSets(JSERR_RegExpInvalidEscape);
                        return;
                    default:
                        if (standardEncodedChars->IsDigit(ECLookahead()))
                        {
                            ECConsume();
                            DeferredFailIfUnicodeSets(JSERR_RegExpInvalidEscape);
                            return;
                        }
                        ECConsume();
                        altCodePoints++;
                        continue;
                }
            }
            // Raw character: syntax characters and reserved pairs rejected.
            if (this->vScanSpeculative && next >= this->vScanLimit)
            {
                throw VScanAbort();
            }
            Char c = NextChar();
            if (VDetail::IsClassSetSyntaxChar(c))
            {
                DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
                return;
            }
            if (VDetail::IsReservedPunctuatorChar(c) && !IsEOF() && ECLookahead() != 0)
            {
                const EncodedChar* afterQ = next;
                const size_t savedMultiUnitsQ = this->GetMultiUnits();
                Char c2Q = NextChar();
                bool isPairQ = c2Q == c;
                next = afterQ;
                this->RestoreMultiUnits(savedMultiUnitsQ);
                if (isPairQ)
                {
                    DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
                    return;
                }
            }
            altCodePoints++;
        }
    }

    // ---------------------------------------------------------------------------
    // Pass 1 (flags known, v on): parse and build
    // ---------------------------------------------------------------------------

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::VClassPass1()
    {
        VCSet* set = Anew(ctAllocator, VCSet);
        VInitSet(*set);
        bool negation = false;
        VParseClass(*set, negation);
        // VParseClass consumed through ']' (nested classes need that), but
        // the TermPass1 caller expects to consume the top-level ']' itself
        // exactly like the legacy CharacterClassPass1 contract.
        Assert(ECLookback(1) == ']');
        ECRevert(1);
        // Any surrogate-pair entries the pass 0 legacy scan tracked inside
        // this class's raw span were consumed here directly as code points;
        // skip them so later atoms still line up with the tracker list (the
        // list is in textual order).
        while (this->currentSurrogatePairNode != nullptr && this->currentSurrogatePairNode->location < next)
        {
            this->currentSurrogatePairNode = this->currentSurrogatePairNode->next;
        }
        if (negation && set->mayContainStrings)
        {
            // Pass 0 already deferred this error (it fires before pass 1);
            // this is a defensive check.
            Fail(JSERR_RegExpSyntax);
        }
        return VBuildNode(*set, negation);
    }

    // Parses a whole class (cursor just after '['); consumes through ']'.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VParseClass(VCSet& set, bool& negation)
    {
        PROBE_STACK_NO_DISPOSE(scriptContext, Js::Constants::MinStackRegex);
        negation = false;
        if (IsEOF() || ECLookahead() == 0)
            Fail(JSERR_RegExpNoBracket);
        if (ECLookahead() == '^')
        {
            negation = true;
            ECConsume();
        }
        if (ECLookahead() != ']')
            VParseExpr(set);
        if (IsEOF() || ECLookahead() != ']')
            Fail(JSERR_RegExpNoBracket);
        ECConsume(); // ]
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VParseExpr(VCSet& acc)
    {
        int unionItems = 0;
        bool sawRange = false;
        char chainOp = 0;

        // Per-level sets live in the compile-time arena (wholesale freed
        // with the parser, like every other v buffer) so deep class nesting
        // consumes only small parser frames; the stack probe in
        // VParseClass then bounds recursion cleanly.
        VCSet* item = Anew(ctAllocator, VCSet);
        VInitSet(*item);
        VParseItem(*item);
        acc = *item;
        sawRange = item->isRange;

        while (true)
        {
            if (IsEOF() || ECLookahead() == 0 || ECLookahead() == ']')
                break;
            bool isAndOp = ECLookahead() == '&' && ECLookahead(1) == '&';
            bool isDiffOp = ECLookahead() == '-' && ECLookahead(1) == '-';
            if (isAndOp || isDiffOp)
            {
                if (unionItems != 0 || sawRange || (chainOp != 0 && chainOp != (isAndOp ? '&' : '-')))
                    Fail(JSERR_RegExpSyntax);
                chainOp = isAndOp ? '&' : '-';
                ECConsume(2);
                // A third operator char ([a&&&], [a---b]): reserved double
                // punctuator (pass 0 deferred it already).
                if (ECLookahead() == chainOp && ECLookahead(1) != 0 && !IsEOF())
                {
                    Fail(JSERR_RegExpSyntax);
                }
                VCSet* rhs = Anew(ctAllocator, VCSet);
                VInitSet(*rhs);
                VParseOperand(*rhs, /*allowRange*/ false);
                VApplyOp(acc, *rhs, /*isDifference*/ chainOp == '-');
                continue;
            }
            if (chainOp != 0)
                Fail(JSERR_RegExpSyntax);
            VCSet* next = Anew(ctAllocator, VCSet);
            VInitSet(*next);
            VParseItem(*next);
            VUnionInPlace(acc, *next);
            unionItems++;
        }
    }

    // Parses one item (operand or char range) into set.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VParseItem(VCSet& set)
    {
        if (IsEOF() || ECLookahead() == 0)
            Fail(JSERR_RegExpNoBracket); // truncated class
        bool setValued =
            (ECLookahead() == '\\' &&
             ((ECLookahead(1) == 'q' && ECLookahead(2) == '{') ||
              ECLookahead(1) == 'd' || ECLookahead(1) == 'D' ||
              ECLookahead(1) == 's' || ECLookahead(1) == 'S' ||
              ECLookahead(1) == 'w' || ECLookahead(1) == 'W' ||
              ((ECLookahead(1) == 'p' || ECLookahead(1) == 'P') && ECLookahead(2) == '{'))) ||
            ECLookahead() == '[';
        if (setValued)
        {
            VParseOperand(set, /*allowRange*/ true);
            // A nested class operand is not a character range even when its
            // own contents include one (the copy carries the inner flag).
            set.isRange = false;
            return;
        }
        // Singleton (range-able) item
        codepoint_t left;
        if (ECLookahead() == '\\')
        {
            ECConsume();
            left = VDecodeEscapeCodePoint();
        }
        else
        {
            left = VReadRawCodePoint();
            if (left == (codepoint_t)'-')
                Fail(JSERR_RegExpSyntax); // unescaped '-'
        }
        if (ECLookahead() == '-' && ECLookahead(1) != ']' && ECLookahead(1) != '-' && ECLookahead(1) != 0)
        {
            ECConsume(); // -
            codepoint_t right;
            if (ECLookahead() == '\\')
            {
                ECConsume();
                right = VDecodeEscapeCodePoint();
            }
            else
            {
                right = VReadRawCodePoint();
                if (right == (codepoint_t)'-')
                    Fail(JSERR_RegExpSyntax);
            }
            if (right < left)
                Fail(JSERR_RegExpBadRange);
            set.cp.SetRange(ctAllocator, left, right);
            set.isRange = true;
            VCloseOperandIfInsensitive(set);
            return;
        }
        VAddCodePoint(set, left);
        VCloseOperandIfInsensitive(set);
    }

    // Parses one operand: nested class, \q{...}, \p{...}, \d&co, escape or
    // raw char. With allowRange false (set-operation right operand), a
    // following range is rejected.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VParseOperand(VCSet& set, bool allowRange)
    {
        if (IsEOF() || ECLookahead() == 0)
            Fail(JSERR_RegExpNoBracket); // truncated class
        if (ECLookahead() == '[')
        {
            ECConsume(); // [
            VCSet* inner = Anew(ctAllocator, VCSet);
            VInitSet(*inner);
            bool innerNegation = false;
            VParseClass(*inner, innerNegation);
            if (innerNegation)
            {
                if (inner->mayContainStrings)
                    Fail(JSERR_RegExpSyntax);
                // closure-then-complement, mirroring the legacy class path
                if (caseInsensitiveFlagPresent)
                {
                    CharSet<codepoint_t> closed;
                    inner->cp.ToEquivClass(ctAllocator, closed);
                    inner->cp.Clear(ctAllocator);
                    inner->cp.CloneFrom(ctAllocator, closed);
                    closed.FreeBody(ctAllocator);
                }
                CharSet<codepoint_t> complemented;
                inner->cp.ToComplement(ctAllocator, complemented);
                inner->cp.Clear(ctAllocator);
                inner->cp.CloneFrom(ctAllocator, complemented);
                complemented.FreeBody(ctAllocator);
            }
            set = *inner;
            VRejectRangeHere();
            return;
        }
        if (ECLookahead() == '\\')
        {
            EncodedChar e1 = ECLookahead(1);
            if (e1 == 'q' && ECLookahead(2) == '{')
            {
                ECConsume(2); // \q
                VParseQ(set);
                VRejectRangeHere();
                return;
            }
            if ((e1 == 'p' || e1 == 'P') && ECLookahead(2) == '{')
            {
                VParsePropertyEscape(set, e1 == 'P');
                VRejectRangeHere();
                return;
            }
            if (e1 == 'd' || e1 == 'D' || e1 == 's' || e1 == 'S' || e1 == 'w' || e1 == 'W')
            {
                ECConsume(2);
                // The standard character sets are exposed in the char16
                // domain; copy their ranges into the code point domain.
                CharSet<Char> units;
                switch (e1)
                {
                    case 'd': standardChars->SetDigits(ctAllocator, units); break;
                    case 'D': standardChars->SetNonDigits(ctAllocator, units); break;
                    case 's': standardChars->SetWhitespace(ctAllocator, units); break;
                    case 'S': standardChars->SetNonWhitespace(ctAllocator, units); break;
                    case 'w':
                        if (unicodeFlagPresent && caseInsensitiveFlagPresent)
                            standardChars->SetWordIUChars(ctAllocator, units);
                        else
                            standardChars->SetWordChars(ctAllocator, units);
                        break;
                    case 'W':
                        if (unicodeFlagPresent && caseInsensitiveFlagPresent)
                            standardChars->SetNonWordIUChars(ctAllocator, units);
                        else
                            standardChars->SetNonWordChars(ctAllocator, units);
                        break;
                }
                Char lo = 0, hi = 0;
                Char search = 0;
                while (units.GetNextRange(search, &lo, &hi))
                {
                    set.cp.SetRange(ctAllocator, lo, hi);
                    if (hi == Chars<Char>::MaxUChar)
                        break; // a complement-style set reaches 0xFFFF; hi + 1 would wrap the char16 search cursor and loop forever
                    search = hi + 1;
                }
                units.FreeBody(ctAllocator);
                // The complement shorthands (\D, \S, \W) are defined over
                // all code points: the char16 set only carries the BMP part,
                // so add the astral planes ([^\D] must equal \d and match
                // no astral input; Node-verified).
                if (e1 == 'D' || e1 == 'S' || e1 == 'W')
                {
                    set.cp.SetRange(ctAllocator, 0x10000, 0x10FFFF);
                }
                VCloseOperandIfInsensitive(set);
                VRejectRangeHere();
                return;
            }
            ECConsume(); // backslash
            codepoint_t cp = VDecodeEscapeCodePoint();
            if (ECLookahead() == '-' && ECLookahead(1) != ']' && ECLookahead(1) != '-' && ECLookahead(1) != 0)
            {
                if (!allowRange)
                    Fail(JSERR_UnicodeRegExpRangeContainsCharClass);
                ECConsume(); // -
                codepoint_t right;
                if (ECLookahead() == '\\')
                {
                    ECConsume();
                    right = VDecodeEscapeCodePoint();
                }
                else
                    right = VReadRawCodePoint();
                if (right == (codepoint_t)'-')
                    Fail(JSERR_RegExpSyntax);
                if (right < cp)
                    Fail(JSERR_RegExpBadRange);
                set.cp.SetRange(ctAllocator, cp, right);
                set.isRange = true;
                VCloseOperandIfInsensitive(set);
                return;
            }
            VAddCodePoint(set, cp);
            VCloseOperandIfInsensitive(set);
            return;
        }
        // Raw character
        codepoint_t cp = VReadRawCodePoint();
        if (cp == (codepoint_t)'-')
            Fail(JSERR_RegExpSyntax); // unescaped '-'
        if (ECLookahead() == '-' && ECLookahead(1) != ']' && ECLookahead(1) != '-' && ECLookahead(1) != 0)
        {
            if (!allowRange)
                Fail(JSERR_UnicodeRegExpRangeContainsCharClass);
            ECConsume(); // -
            codepoint_t right;
            if (ECLookahead() == '\\')
            {
                ECConsume();
                right = VDecodeEscapeCodePoint();
            }
            else
            {
                right = VReadRawCodePoint();
                if (right == (codepoint_t)'-')
                    Fail(JSERR_RegExpSyntax);
            }
            if (right < cp)
                Fail(JSERR_RegExpBadRange);
            set.cp.SetRange(ctAllocator, cp, right);
            set.isRange = true;
            VCloseOperandIfInsensitive(set);
            return;
        }
        VAddCodePoint(set, cp);
        VCloseOperandIfInsensitive(set);
    }

    // After a set-valued operand (\q, \p, \d&co, nested class): a '-' that
    // is neither the difference operator nor the class end would make the
    // operand a range endpoint, which is a SyntaxError under v (as under u).
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VRejectRangeHere()
    {
        if (ECLookahead() == '-' && ECLookahead(1) != ']' && ECLookahead(1) != '-' && ECLookahead(1) != 0)
        {
            Fail(JSERR_UnicodeRegExpRangeContainsCharClass);
        }
    }

    // Decodes one singleton escape (cursor after the backslash). v rules:
    // octal escapes other than \0 are rejected, as are the class/set-valued
    // escape letters outside their operand forms; identity escapes follow
    // the u-mode class policy (punctuation; not word characters).
    template <typename P, const bool IsLiteral>
    codepoint_t Parser<P, IsLiteral>::VDecodeSingletonEscape(bool inQString)
    {
        Assert(!IsEOF());
        EncodedChar ec = ECLookahead();
        if (standardEncodedChars->IsDigit(ec))
        {
            if (ec != '0')
                Fail(JSERR_RegExpInvalidEscape);
            ECConsume();
            // \0 not followed by another digit (would be an octal escape)
            if (!IsEOF() && ECLookahead() != 0 && standardEncodedChars->IsDigit(ECLookahead()))
                Fail(JSERR_RegExpInvalidEscape);
            return 0;
        }
        Char c = NextChar();
        switch (c)
        {
            case 'f': return '\f';
            case 'n': return '\n';
            case 'r': return '\r';
            case 't': return '\t';
            case 'v': return '\v';
            case 'b': return '\b';
            case 'c':
            {
                if (!IsEOF() && ECLookahead() != 0 && standardEncodedChars->IsLetter(ECLookahead()))
                {
                    codepoint_t v = Chars<EncodedChar>::CTU(ECLookahead()) % 32;
                    ECConsume();
                    return v;
                }
                Fail(JSERR_RegExpInvalidEscape);
                return 0;
            }
            case 'x':
            {
                if (ECCanConsume(2) && standardEncodedChars->IsHex(ECLookahead(0)) && standardEncodedChars->IsHex(ECLookahead(1)))
                {
                    codepoint_t v = (standardEncodedChars->DigitValue(ECLookahead(0)) << 4) |
                        standardEncodedChars->DigitValue(ECLookahead(1));
                    ECConsume(2);
                    return v;
                }
                Fail(JSERR_RegExpInvalidEscape);
                return 0;
            }
            case 'u':
                return VDecodeUEscape();
            case 'q': case 'p': case 'P': case 'k':
            case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
                Fail(JSERR_RegExpInvalidEscape);
                return 0;
            default:
                // Identity escape; v (like u) rejects word characters
                // (letters, digits, '_'), which cover the practical
                // UnicodeIDContinue set for validation purposes. Escaped
                // '}' and '|' are valid inside \q (only the raw characters
                // terminate an alternative).
                if (standardChars->IsWord(c))
                    Fail(JSERR_RegExpInvalidEscape);
                return c;
        }
    }

    // Decodes one singleton escape (cursor after the backslash) and applies
    // the u-mode pattern rule that an escaped high surrogate followed by an
    // escaped \uXXXX low surrogate combines into one astral code point, so
    // [\uD83D\uDE00-\uD83D\uDE01] forms the astral range U+1F600..U+1F601
    // (Node-verified). The pair is only combined when both halves arrive as
    // \uXXXX escapes (the raw-text case is handled by VReadRawCodePoint).
    template <typename P, const bool IsLiteral>
    codepoint_t Parser<P, IsLiteral>::VDecodeEscapeCodePoint()
    {
        codepoint_t cp = VDecodeSingletonEscape(/*inQString*/ false);
        if (cp >= 0xD800 && cp <= 0xDBFF && ECLookahead() == '\\' && ECLookahead(1) == 'u')
        {
            const EncodedChar* beforeSecond = next;
            ECConsume(2); // \u
            codepoint_t second = VDecodeUEscape();
            if (second >= 0xDC00 && second <= 0xDFFF)
                return VDetail::SurrogatePairToCodePoint((Char)cp, (Char)second);
            next = beforeSecond; // not a pair; rewind the second escape
        }
        return cp;
    }

    // \u{...} or \uHHHH with the 'u' consumed; returns the whole code point.
    template <typename P, const bool IsLiteral>
    codepoint_t Parser<P, IsLiteral>::VDecodeUEscape()
    {
        if (ECCanConsume(1) && ECLookahead(0) == '{')
        {
            codepoint_t cp = 0;
            ECConsume(); // {
            CharCount digits = 0;
            while (!IsEOF() && ECLookahead() != 0 && standardEncodedChars->IsHex(ECLookahead()))
            {
                cp = cp * 16 + standardEncodedChars->DigitValue(ECLookahead());
                if (cp > 0x10FFFF)
                    Fail(JSERR_RegExpInvalidEscape);
                digits++;
                ECConsume();
            }
            if (digits == 0 || IsEOF() || ECLookahead() != '}')
                Fail(JSERR_RegExpSyntax);
            ECConsume(); // }
            return cp;
        }
        if (ECCanConsume(4) &&
            standardEncodedChars->IsHex(ECLookahead(0)) && standardEncodedChars->IsHex(ECLookahead(1)) &&
            standardEncodedChars->IsHex(ECLookahead(2)) && standardEncodedChars->IsHex(ECLookahead(3)))
        {
            codepoint_t v = (standardEncodedChars->DigitValue(ECLookahead(0)) << 12) |
                (standardEncodedChars->DigitValue(ECLookahead(1)) << 8) |
                (standardEncodedChars->DigitValue(ECLookahead(2)) << 4) |
                standardEncodedChars->DigitValue(ECLookahead(3));
            ECConsume(4);
            return v;
        }
        Fail(JSERR_RegExpInvalidEscape);
        return 0;
    }

    // Reads one raw code point, pairing adjacent surrogate units (v classes
    // are code-point based and the flags are known here, so no deferred
    // tracking is needed).
    template <typename P, const bool IsLiteral>
    codepoint_t Parser<P, IsLiteral>::VReadRawCodePoint()
    {
        if (IsEOF() || ECLookahead() == 0)
            Fail(JSERR_RegExpNoBracket); // truncated class (debug guard: callers used to assert here)
        Char c = NextChar();
        if (VDetail::IsHighSurrogateUnit(c) && !IsEOF() && ECLookahead() != 0)
        {
            const EncodedChar* after = next;
            Char c2 = NextChar();
            if (VDetail::IsLowSurrogateUnit(c2))
                return VDetail::SurrogatePairToCodePoint(c, c2);
            next = after; // not a pair; rewind the second unit
        }
        return c;
    }

    // \q{alt1|alt2|...} with '{' at the cursor ('q' consumed).
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VParseQ(VCSet& set)
    {
        Assert(ECLookahead() == '{');
        ECConsume(); // {
        while (true)
        {
            if (IsEOF() || ECLookahead() == 0)
                Fail(JSERR_RegExpSyntax);
            // One alternative: decode code points into scratch. An empty
            // alternative (including \q{} itself) becomes the empty-string
            // member through VAddStringFromCodePoints.
            CharCount numCps = 0;
            while (true)
            {
                if (IsEOF() || ECLookahead() == 0)
                    Fail(JSERR_RegExpSyntax);
                if (ECLookahead() == '}' || ECLookahead() == '|')
                    break;
                codepoint_t cp;
                if (ECLookahead() == '\\')
                {
                    ECConsume();
                    cp = VDecodeSingletonEscape(/*inQString*/ true);
                    // Escaped surrogate halves may combine with the next
                    // \uXXXX escape (the u-mode pattern rule).
                    if (cp >= 0xD800 && cp <= 0xDBFF && ECLookahead() == '\\' && ECLookahead(1) == 'u')
                    {
                        const EncodedChar* beforeSecond = next;
                        ECConsume(2); // \u
                        codepoint_t second = VDecodeUEscape();
                        if (second >= 0xDC00 && second <= 0xDFFF)
                            cp = VDetail::SurrogatePairToCodePoint((Char)cp, (Char)second);
                        else
                            next = beforeSecond;
                    }
                }
                else
                {
                    cp = VReadRawCodePoint();
                }
                if (numCps == vScratchCpsCap)
                {
                    CharCount newCap = vScratchCpsCap == 0 ? 16 : vScratchCpsCap * 2;
                    codepoint_t* newList = AnewArray(ctAllocator, codepoint_t, newCap);
                    for (CharCount i = 0; i < numCps; i++)
                        newList[i] = vScratchCps[i];
                    vScratchCps = newList;
                    vScratchCpsCap = newCap;
                }
                vScratchCps[numCps++] = cp;
            }
            VAddStringFromCodePoints(set, vScratchCps, numCps);
            if (ECLookahead() == '}')
            {
                ECConsume();
                return;
            }
            if (ECLookahead() == '|')
                ECConsume();
            else
                Fail(JSERR_RegExpSyntax);
        }
    }

    // \p{...}/\P{...} operand (cursor at the escape start).
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::VParsePropertyEscape(VCSet& set, bool isNegation)
    {
        ECConsume(2); // \p or \P
        if (IsEOF() || ECLookahead() != '{')
            Fail(JSERR_RegExpInvalidEscape);
        char16 name[propertyEscapeMaxNameLen];
        char16 value[propertyEscapeMaxNameLen];
        CharCount nameLen, valueLen;
        bool hasValue;
        if (!ScanPropertyEscapeBody(name, nameLen, value, valueLen, hasValue))
            Fail(JSERR_RegExpSyntax);
        UnicodeProperty::Resolved resolved;
        if (!UnicodeProperty::Resolve(name, nameLen, value, valueLen, hasValue, &resolved))
            Fail(JSERR_RegExpSyntax);

        if (UnicodeProperty::IsStringProperty(resolved))
        {
            if (isNegation)
                Fail(JSERR_RegExpSyntax); // pass 0 already deferred this
            // Collector appends the ICU strings as members.
            struct Collector : UnicodeProperty::StringCollector
            {
                Parser* parser;
                VCSet* set;
                virtual void Add(const char16* s, int32 len) override
                {
                    CharCount cps = 0;
                    for (int32 i = 0; i < len; )
                    {
                        if (i + 1 < len && VDetail::IsHighSurrogateUnit(s[i]) && VDetail::IsLowSurrogateUnit(s[i + 1]))
                            i += 2;
                        else
                            i += 1;
                        cps++;
                    }
                    if (cps > parser->vScratchCpsCap)
                    {
                        CharCount newCap = parser->vScratchCpsCap == 0 ? 16 : parser->vScratchCpsCap;
                        while (cps > newCap)
                            newCap *= 2;
                        codepoint_t* newList = AnewArray(parser->ctAllocator, codepoint_t, newCap);
                        for (CharCount i = 0; i < cps; i++)
                            newList[i] = 0;
                        parser->vScratchCps = newList;
                        parser->vScratchCpsCap = newCap;
                    }
                    CharCount n = 0;
                    for (int32 i = 0; i < len; )
                    {
                        if (i + 1 < len && VDetail::IsHighSurrogateUnit(s[i]) && VDetail::IsLowSurrogateUnit(s[i + 1]))
                        {
                            parser->vScratchCps[n++] = VDetail::SurrogatePairToCodePoint(s[i], s[i + 1]);
                            i += 2;
                        }
                        else
                        {
                            parser->vScratchCps[n++] = s[i];
                            i += 1;
                        }
                    }
                    parser->VAddStringFromCodePoints(*set, parser->vScratchCps, n);
                }
            } collector;
            collector.parser = this;
            collector.set = &set;
            if (!UnicodeProperty::BuildStringPropertySet(resolved, ctAllocator, set.cp, &collector))
                Fail(JSERR_RegExpSyntax); // ICU failed
            return;
        }

        CharSet<codepoint_t> codePointSet;
        if (!UnicodeProperty::BuildCodePointSet(resolved, ctAllocator, codePointSet))
            Fail(JSERR_RegExpSyntax);
        if (!isNegation)
        {
            set.cp.UnionInPlace(ctAllocator, codePointSet);
            codePointSet.FreeBody(ctAllocator);
        }
        else
        {
            CharSet<codepoint_t> complemented;
            codePointSet.ToComplement(ctAllocator, complemented);
            codePointSet.FreeBody(ctAllocator);
            set.cp.UnionInPlace(ctAllocator, complemented);
            complemented.FreeBody(ctAllocator);
        }
        // Close the property operand so &&/-- see closed operands under i
        // (the ICU set arrives raw). String-property members were added
        // above and use the canonical string domain; closing them would
        // over-fold, so only close when no string members were added.
        if (set.numStrings == 0 && !set.containsEmptyString)
        {
            VCloseOperandIfInsensitive(set);
        }
    }

    // Builds the final node: an alternation trying the string members
    // longest-first (each a literal), then the code point set, then (if the
    // empty string is a member) a zero-width success.
    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::VBuildNode(VCSet& set, bool negation)
    {
        // Under i every operand was already closed at its insertion point
        // (ordinary operands with the fold closure, \q members with the
        // spec-canonical equivalence), so no whole-set closure happens
        // here; re-closing would over-fold the canonical \q members (e.g.
        // admit U+212A for a 'k' member). Complement for negated classes
        // (the complement of a closed set is closed).
        if (negation)
        {
            Assert(!set.mayContainStrings && set.numStrings == 0 && !set.containsEmptyString);
            CharSet<codepoint_t> complemented;
            set.cp.ToComplement(ctAllocator, complemented);
            set.cp.Clear(ctAllocator);
            set.cp.CloneFrom(ctAllocator, complemented);
            complemented.FreeBody(ctAllocator);
        }

        // Sort string members longest (code points) first; order among
        // equal lengths is unobservable (equal consumption on match).
        // Duplicate members (repeated \q alternatives or overlapping
        // property-of-strings sets) are removed after the sort: equal-length
        // adjacent duplicates are unobservable, so this is purely a size
        // optimization, bounded by the sort itself.
        if (set.numStrings > 1)
        {
            uint32 maxLen = 0;
            for (int i = 0; i < set.numStrings; i++)
                maxLen = max(maxLen, set.strings[i].cpLen);
            VString* tmp = AnewArray(ctAllocator, VString, set.numStrings);
            CharCount* counts = AnewArrayZ(ctAllocator, CharCount, maxLen + 2);
            for (int i = 0; i < set.numStrings; i++)
                counts[set.strings[i].cpLen]++;
            // descending: the position for length L is the count of all
            // strictly longer strings
            CharCount running = 0;
            for (uint32 len = maxLen + 1; len-- > 0; )
            {
                CharCount c = counts[len];
                counts[len] = running;
                running += c;
            }
            for (int i = 0; i < set.numStrings; i++)
                tmp[counts[set.strings[i].cpLen]++] = set.strings[i];
            for (int i = 0; i < set.numStrings; i++)
                set.strings[i] = tmp[i];

            int outCount = 1;
            for (int i = 1; i < set.numStrings; i++)
            {
                if (!VStringEq(set.strings[outCount - 1], set.strings[i]))
                    set.strings[outCount++] = set.strings[i];
            }
            set.numStrings = outCount;
        }

        Node* armsHead = nullptr;
        AltNode* armsLast = nullptr;
        auto appendArm = [&](Node* arm)
        {
            if (armsHead == nullptr)
            {
                armsHead = arm;
                return;
            }
            AltNode* link = Anew(ctAllocator, AltNode, arm, nullptr);
            if (armsLast == nullptr)
            {
                armsLast = link;
                armsHead = Anew(ctAllocator, AltNode, armsHead, armsLast);
            }
            else
            {
                armsLast->tail = link;
                armsLast = link;
            }
        };

        for (int i = 0; i < set.numStrings; i++)
        {
            const VString& s = set.strings[i];
            Assert(s.unitLen > 1);
            EnsureLitbuf(s.unitLen);
            for (CharCount j = 0; j < s.unitLen; j++)
                litbuf[litbufNext + j] = s.chars[j];
            // Under i the stored bytes are the spec-Canonicalize form of
            // each code point; the canonical instruction compares
            // Canonicalize(input) with them (never over-folding U+212A /
            // U+017F the way the equiv-class expansion does).
            MatchLiteralNode* lit = Anew(ctAllocator, MatchLiteralNode, litbufNext, s.unitLen, caseInsensitiveFlagPresent);
            if (caseInsensitiveFlagPresent)
                lit->isCanonical = true;
            litbufNext += s.unitLen;
            appendArm(lit);
        }

        if (!set.cp.IsEmpty() || (set.numStrings == 0 && !set.containsEmptyString))
        {
            // TranslateCodePointSet clears the set body. A negated v class
            // (pure code points: contents that may contain strings are
            // rejected earlier) keeps the negated-class half-pair rule.
            appendArm(TranslateCodePointSet(set.cp, /*isNegation*/ false, /*noHalfPairFallback*/ negation));
        }
        else
        {
            set.cp.FreeBody(ctAllocator);
        }

        if (set.containsEmptyString)
        {
            appendArm(Anew(ctAllocator, SimpleNode, Node::Empty));
        }

        return armsHead != nullptr ? armsHead : Anew(ctAllocator, SimpleNode, Node::Empty);
    }

    //
    // Errors
    //

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::Fail(HRESULT error)
    {
        throw ParseError(inBody, Pos(), Chars<EncodedChar>::OSB(next, input), error);
    }

    // This doesn't throw, but stores first error code for throwing later
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::DeferredFailIfUnicode(HRESULT error)
    {
        Assert(this->scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled());
        if (this->deferredIfUnicodeError == nullptr)
        {
            this->deferredIfUnicodeError = Anew(ctAllocator, ParseError, inBody, Pos(), Chars<EncodedChar>::OSB(next, input), error);
        }
    }

    // This doesn't throw, but stores first error code for throwing later
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::DeferredFailIfNotUnicode(HRESULT error)
    {
        Assert(this->scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled());
        if (this->deferredIfNotUnicodeError == nullptr)
        {
            this->deferredIfNotUnicodeError = Anew(ctAllocator, ParseError, inBody, Pos(), Chars<EncodedChar>::OSB(next, input), error);
        }
    }

    // Pass 0 helper for atom decimal escapes: the cursor is at the first
    // digit after '\'. Consumes nothing (the caller's digit loop owns the
    // cursor); records value/shape for the entry points' u-mode validation.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::RecordDecimalEscapePass0()
    {
        if (this->numDecimalEscapes == this->decimalEscapesCap)
        {
            int newCap = this->decimalEscapesCap == 0 ? 8 : this->decimalEscapesCap * 2;
            DecimalEscapeOccurrence* newList = AnewArray(ctAllocator, DecimalEscapeOccurrence, newCap);
            for (int i = 0; i < this->numDecimalEscapes; i++)
                newList[i] = this->decimalEscapes[i];
            this->decimalEscapes = newList;
            this->decimalEscapesCap = newCap;
        }
        DecimalEscapeOccurrence& occ = this->decimalEscapes[this->numDecimalEscapes++];
        // Mirror pass 1's five-digit accumulation so the recorded value and
        // its overlong flag agree with the backreference/octal
        // disambiguation the non-u path performs.
        CharCount digits = 0;
        uint32 value = 0;
        while (digits < 5 && ECCanConsume(digits + 1) && standardEncodedChars->IsDigit(ECLookahead(digits)))
        {
            value = value * 10 + (uint32)standardEncodedChars->DigitValue(ECLookahead(digits));
            digits++;
        }
        occ.value = value;
        occ.pos = Pos();
        occ.encodedPos = Chars<EncodedChar>::OSB(next, input);
        occ.zeroLeadingRun = ECLookahead() == '0' && ECCanConsume(2) && standardEncodedChars->IsDigit(ECLookahead(1));
        occ.overlong = digits == 5 && ECCanConsume(digits + 1) && standardEncodedChars->IsDigit(ECLookahead(digits));
    }

    // Called by every entry point once the flags and the final group count
    // are known: under u/v an atom decimal escape must be \0 or a
    // backreference to an existing (possibly later-defined) group. Without
    // the flag the Annex B octal/identity readings in pass 1 are untouched.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::ValidateDecimalEscapesIfUnicode()
    {
        if (!this->unicodeFlagPresent)
        {
            return;
        }
        for (int i = 0; i < this->numDecimalEscapes; i++)
        {
            const DecimalEscapeOccurrence& occ = this->decimalEscapes[i];
            if (occ.zeroLeadingRun || occ.overlong || occ.value >= (uint32)this->numGroups)
            {
                throw ParseError(true, occ.pos, occ.encodedPos, JSERR_RegExpInvalidEscape);
            }
        }
    }

    // v-mode (Unicode sets) deferred errors; see RegexParser.h. Recorded in
    // pass 0, raised by the entry points once the flags are known.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::DeferredFailIfUnicodeSets(HRESULT error)
    {
        if (this->deferredIfUnicodeSetsError == nullptr)
        {
            this->deferredIfUnicodeSetsError = Anew(ctAllocator, ParseError, inBody, Pos(), Chars<EncodedChar>::OSB(next, input), error);
        }
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::DeferredFailIfUnicodeNoSets(HRESULT error)
    {
        if (this->deferredIfUnicodeNoSetsError == nullptr)
        {
            this->deferredIfUnicodeNoSetsError = Anew(ctAllocator, ParseError, inBody, Pos(), Chars<EncodedChar>::OSB(next, input), error);
        }
    }

    template <typename P, const bool IsLiteral>
    inline void Parser<P, IsLiteral>::ECMust(EncodedChar ec, HRESULT error)
    {
        // We never look for 0
        Assert(ec != 0);
        if (ECLookahead() != ec)
            Fail(error);
        ECConsume();
    }

    template <typename P, const bool IsLiteral>
    inline char16 Parser<P, IsLiteral>::NextChar()
    {
        Assert(!IsEOF());
        // Could be an embedded 0
        Char c = this->template ReadFull<true>(next, inputLim);
        // No embedded newlines in literals
        if (IsLiteral && standardChars->IsNewline(c))
        {
            if (this->vScanSpeculative)
            {
                // The pre-flag v-class validation is speculative: the same
                // text may be a legal legacy class (e.g. /[[]/). Reaching a
                // newline means the v interpretation ran past the literal;
                // record the v-only error and abort this scan, leaving the
                // legacy pass to parse the text.
                DeferredFailIfUnicodeSets(JSERR_RegExpNoBracket);
                throw VScanAbort();
            }
            Fail(ERRnoSlash);
        }
        return c;
    }

    //
    // Named capture groups (ES2018)
    //

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::EnsureNameBuffer(CharCount additionalChars)
    {
        CharCount newCap = namedGroupBufferCap;
        if (namedGroupBuffer != nullptr && namedGroupBufferNext + additionalChars <= namedGroupBufferCap)
        {
            return;
        }
        if (namedGroupBuffer == nullptr)
        {
            newCap = max<CharCount>(16, additionalChars);
        }
        else
        {
            if (additionalChars > MaxCharCount - namedGroupBufferNext)
                Fail(JSERR_RegExpSyntax);
            while (namedGroupBufferNext + additionalChars > newCap)
                newCap *= 2;
        }
        Char* newBuffer = AnewArray(ctAllocator, Char, newCap);
        if (namedGroupBuffer != nullptr)
        {
            for (CharCount i = 0; i < namedGroupBufferNext; i++)
                newBuffer[i] = namedGroupBuffer[i];
        }
        namedGroupBuffer = newBuffer;
        namedGroupBufferCap = newCap;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::AppendNameCodePoint(codepoint_t codePoint)
    {
        if (codePoint <= 0xFFFF)
        {
            EnsureNameBuffer(1);
            namedGroupBuffer[namedGroupBufferNext++] = static_cast<Char>(codePoint);
        }
        else
        {
            EnsureNameBuffer(2);
            codePoint -= 0x10000;
            namedGroupBuffer[namedGroupBufferNext++] = static_cast<Char>(0xD800 + (codePoint >> 10));
            namedGroupBuffer[namedGroupBufferNext++] = static_cast<Char>(0xDC00 + (codePoint & 0x3FF));
        }
    }

    // Parses \uXXXX or \u{X...} with the leading "u" already consumed.
    template <typename P, const bool IsLiteral>
    codepoint_t Parser<P, IsLiteral>::ScanNameUnicodeEscape()
    {
        codepoint_t value = 0;
        if (ECLookahead() == '{')
        {
            ECConsume();
            CharCount digits = 0;
            while (!IsEOF() && standardEncodedChars->IsHex(ECLookahead()))
            {
                value = value * 16 + standardEncodedChars->DigitValue(ECLookahead());
                if (value > 0x10FFFF)
                    Fail(JSERR_RegExpSyntax);
                digits++;
                ECConsume();
            }
            if (digits == 0 || IsEOF() || ECLookahead() != '}')
                Fail(JSERR_RegExpSyntax);
            ECConsume(); // }
            return value;
        }
        for (int i = 0; i < 4; i++)
        {
            if (IsEOF() || !standardEncodedChars->IsHex(ECLookahead()))
                Fail(JSERR_RegExpSyntax);
            value = value * 16 + standardEncodedChars->DigitValue(ECLookahead());
            ECConsume();
        }
        return value;
    }

    // Scans a RegExpIdentifierName with the position just after "(?<" or
    // "\k<". Consumes through and including the terminating '>'. The decoded
    // name is appended to namedGroupBuffer (caller snapshots
    // namedGroupBufferNext before/after to get its bounds). Fails on invalid
    // name characters, escape sequences, lone surrogates, and empty names.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::ScanGroupName()
    {
        const Js::CharClassifier* classifier = this->scriptContext->GetCharClassifier();
        bool first = true;
        for (;;)
        {
            if (IsEOF())
                Fail(JSERR_RegExpSyntax);
            Char c = NextChar();
            if (c == '>')
                break;

            codepoint_t codePoint;
            if (c == '\\')
            {
                if (IsEOF() || ECLookahead() != 'u')
                    Fail(JSERR_RegExpSyntax);
                ECConsume(); // u
                codePoint = ScanNameUnicodeEscape();
                if (Js::NumberUtilities::IsSurrogateLowerPart(codePoint))
                {
                    // An escaped leading surrogate must pair with another \uXXXX escape
                    if (ECCanConsume(2) && ECLookahead() == '\\' && ECLookahead(1) == 'u')
                    {
                        ECConsume(2);
                        codepoint_t upper = ScanNameUnicodeEscape();
                        if (!Js::NumberUtilities::IsSurrogateUpperPart(upper))
                            Fail(JSERR_RegExpSyntax);
                        codePoint = Js::NumberUtilities::SurrogatePairAsCodePoint(
                            static_cast<char16>(codePoint), static_cast<char16>(upper));
                    }
                    else
                        Fail(JSERR_RegExpSyntax);
                }
                else if (Js::NumberUtilities::IsSurrogateUpperPart(codePoint))
                    Fail(JSERR_RegExpSyntax);
            }
            else if (Js::NumberUtilities::IsSurrogateLowerPart(c))
            {
                if (IsEOF())
                    Fail(JSERR_RegExpSyntax);
                Char c2 = NextChar();
                if (!Js::NumberUtilities::IsSurrogateUpperPart(c2))
                    Fail(JSERR_RegExpSyntax);
                codePoint = Js::NumberUtilities::SurrogatePairAsCodePoint(c, c2);
            }
            else if (Js::NumberUtilities::IsSurrogateUpperPart(c))
            {
                Fail(JSERR_RegExpSyntax);
            }
            else
            {
                codePoint = c;
            }

            if (first ? !classifier->IsIdStart(codePoint) : !classifier->IsIdContinue(codePoint))
                Fail(JSERR_RegExpSyntax);
            first = false;
            AppendNameCodePoint(codePoint);
        }
        if (first)
            Fail(JSERR_RegExpSyntax); // empty name
    }

    template <typename P, const bool IsLiteral>
    bool Parser<P, IsLiteral>::NamedGroupIsAt(const NamedGroup& group, const Char* name, CharCount len) const
    {
        if (group.len != len)
            return false;
        const Char* candidate = namedGroupBuffer + group.offset;
        CharCount j = 0;
        while (j < len && candidate[j] == name[j])
            j++;
        return j == len;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::RecordNamedGroup(const Char* name, CharCount len, int groupId)
    {
        // ES2025 duplicate named groups: a name may be bound again by a
        // later group, but only from a different alternative of some
        // disjunction (no path may pass through two groups of the same
        // name). Declaring into the current alternative's frame rejects
        // duplicates within one alternative, nested or sequential;
        // PopAltScope rejects the ones a merge would reintroduce.
        for (int i = this->altFrameStart; i < this->altPathCount; i++)
        {
            if (NamedGroupIsAt(this->namedGroups[this->altPathGroupIdx[i]], name, len))
                Fail(JSERR_RegExpSyntax);
        }
        if (numNamedGroups == namedGroupsCap)
        {
            int newCap = namedGroupsCap == 0 ? 4 : namedGroupsCap * 2;
            NamedGroup* newList = AnewArray(ctAllocator, NamedGroup, newCap);
            for (int i = 0; i < numNamedGroups; i++)
                newList[i] = namedGroups[i];
            namedGroups = newList;
            namedGroupsCap = newCap;
        }
        NamedGroup& entry = namedGroups[numNamedGroups++];
        entry.offset = name - namedGroupBuffer;
        entry.len = len;
        entry.groupId = groupId;
        EnsureAltPathCapacity();
        this->altPathGroupIdx[this->altPathCount++] = numNamedGroups - 1;
    }

    template <typename P, const bool IsLiteral>
    int Parser<P, IsLiteral>::FindNamedGroup(const Char* name, CharCount len) const
    {
        for (int i = 0; i < numNamedGroups; i++)
        {
            if (NamedGroupIsAt(namedGroups[i], name, len))
                return namedGroups[i].groupId;
        }
        return -1;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::EnsureAltPathCapacity()
    {
        if (this->altPathCount == this->altPathCap)
        {
            int newCap = this->altPathCap == 0 ? 16 : this->altPathCap * 2;
            int* newList = AnewArray(ctAllocator, int, newCap);
            for (int i = 0; i < this->altPathCount; i++)
                newList[i] = this->altPathGroupIdx[i];
            this->altPathGroupIdx = newList;
            this->altPathCap = newCap;
        }
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::EnsureAltScopeCapacity()
    {
        if (this->altScopeCount == this->altScopeCap)
        {
            int newCap = this->altScopeCap == 0 ? 16 : this->altScopeCap * 2;
            AltScope* newList = AnewArray(ctAllocator, AltScope, newCap);
            for (int i = 0; i < this->altScopeCount; i++)
                newList[i] = this->altScopes[i];
            this->altScopes = newList;
            this->altScopeCap = newCap;
        }
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::PushAltScope()
    {
        EnsureAltScopeCapacity();
        AltScope& scope = this->altScopes[this->altScopeCount++];
        scope.savedFrameStart = this->altFrameStart;
        scope.savedAccStart = this->altPathCount;
        this->altFrameStart = this->altPathCount;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::FoldAlternativeIntoScope()
    {
        // The finished alternative's frame joins the disjunction's
        // accumulator (the contiguous region below), and the next
        // alternative starts a fresh frame.
        this->altFrameStart = this->altPathCount;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::PopAltScope()
    {
        const AltScope& scope = this->altScopes[this->altScopeCount - 1];
        // The accumulator holds every name bound anywhere in this
        // disjunction. Merging any of them into the enclosing frame's
        // names would leave two same-name groups on one path.
        for (int i = scope.savedAccStart; i < this->altPathCount; i++)
        {
            const NamedGroup& acc = this->namedGroups[this->altPathGroupIdx[i]];
            for (int j = scope.savedFrameStart; j < scope.savedAccStart; j++)
            {
                if (NamedGroupIsAt(this->namedGroups[this->altPathGroupIdx[j]],
                        namedGroupBuffer + acc.offset, acc.len))
                    Fail(JSERR_RegExpSyntax);
            }
        }
        // The accumulator's entries stay in place and now belong to the
        // enclosing alternative's frame.
        this->altFrameStart = scope.savedFrameStart;
        this->altScopeCount--;
    }

    // Runs after pass 0 and option parsing: "\k" is a backreference escape
    // only when the pattern has named captures or the u flag is present;
    // otherwise it is an Annex B identity escape. Throws on dangling or
    // malformed references (pass 1 then trusts the resolution). 'body'/
    // 'bodyLim' delimit the buffer pass 0 scanned, which holds the recorded
    // spans.
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::ResolveNamedGroupRefs(const EncodedChar* body, const EncodedChar* bodyLim)
    {
        const bool isNamedMode = this->numNamedGroups > 0 || this->unicodeFlagPresent;
        if (!isNamedMode)
            return; // every \k re-parses as the identity escape 'k' in pass 1
        if (this->sawBareK || this->sawClassK)
            throw ParseError(true, 0, 0, JSERR_RegExpSyntax);

        // The spans point into the pass 0 buffer, which is not necessarily
        // the parser's current buffer: ParseDynamic scans the options from a
        // separate string, so IsEOF and the lookahead bounds still refer to
        // that options buffer. Re-bind to the body for the re-scan and
        // restore the caller's position afterwards (ParseLiteral reads it
        // right after this call).
        const EncodedChar* savedInput = this->input;
        const EncodedChar* savedInputLim = this->inputLim;
        const EncodedChar* savedNext = this->next;
        const bool savedInBody = this->inBody;
        const size_t savedMultiUnits = this->GetMultiUnits();
        SetPosition(body, bodyLim, true);

        for (int i = 0; i < this->numNamedGroupRefSpans; i++)
        {
            const NamedGroupRefSpan& span = this->namedGroupRefSpans[i];
            if (span.afterGt == nullptr)
                throw ParseError(true, 0, 0, JSERR_RegExpSyntax);
            this->next = span.nameStart;
            CharCount nameOffset = this->namedGroupBufferNext;
            ScanGroupName(); // validates and consumes through '>'
            if (FindNamedGroup(this->namedGroupBuffer + nameOffset, this->namedGroupBufferNext - nameOffset) < 0)
                throw ParseError(true, 0, 0, JSERR_RegExpSyntax);
        }

        this->input = savedInput;
        this->inputLim = savedInputLim;
        this->next = savedNext;
        this->inBody = savedInBody;
        this->RestoreMultiUnits(savedMultiUnits);
    }

    // Copies the pass 0 name region and definitions into recycler-owned
    // program fields (RegexPattern shares programs across script contexts,
    // so the data must be immutable from here on).
    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::CaptureGroupNames(Recycler* recycler, Program* program)
    {
        Assert(this->numNamedGroups >= 0 && this->numNamedGroups <= MAX_NUM_GROUPS);
        program->numGroupNames = static_cast<uint16>(this->numNamedGroups);
        if (this->numNamedGroups == 0)
            return;
        Assert(this->namedGroupBuffer != nullptr);
        program->groupNameEntries = RecyclerNewArrayLeafZ(recycler, Program::GroupNameEntry, this->numNamedGroups);
        program->groupNameChars = RecyclerNewArrayLeaf(recycler, Char, this->namedGroupBufferLen);
        for (CharCount i = 0; i < this->namedGroupBufferLen; i++)
            program->groupNameChars[i] = this->namedGroupBuffer[i];
        for (int i = 0; i < this->numNamedGroups; i++)
        {
            program->groupNameEntries[i].offset = this->namedGroups[i].offset;
            program->groupNameEntries[i].len = this->namedGroups[i].len;
            program->groupNameEntries[i].groupId = static_cast<uint16>(this->namedGroups[i].groupId);
        }
    }


    //
    // Patterns/Disjunctions/Alternatives
    //

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::PatternPass0()
    {
        this->positionAfterLastSurrogate = nullptr;
        this->deferredIfNotUnicodeError = nullptr;
        this->deferredIfUnicodeError = nullptr;
        DisjunctionPass0(0);
        // Freeze the region of namedGroupBuffer holding pass 0 names; later
        // re-scans (reference resolution, pass 1) append past this point so
        // the recorded definitions stay addressable.
        this->namedGroupBufferLen = this->namedGroupBufferNext;
    }

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::PatternPass1()
    {
        return DisjunctionPass1();
    }

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::UnionNodes(Node* prev, Node* curr)
    {
        if (prev->tag == Node::MatchChar)
        {
            MatchCharNode* prevChar = (MatchCharNode*)prev;
            if (curr->tag == Node::MatchChar)
            {
                MatchCharNode* currChar = (MatchCharNode*)curr;
                if (prevChar->cs[0] == currChar->cs[0])
                    // Just ignore current node
                    return prevChar;
                else
                {
                    // Union chars into new set
                    MatchSetNode* setNode = Anew(ctAllocator, MatchSetNode, false, true, prevChar->isCaseInsensitive);
                    setNode->set.Set(ctAllocator, prevChar->cs[0]);
                    setNode->set.Set(ctAllocator, currChar->cs[0]);
                    return setNode;
                }
            }
            else if (curr->tag == Node::MatchSet)
            {
                MatchSetNode* currSet = (MatchSetNode*)curr;
                if (currSet->isNegation)
                    // Can't merge
                    return 0;
                else
                {
                    // Union chars into new set
                    MatchSetNode* setNode = Anew(ctAllocator, MatchSetNode, false, true, prevChar->isCaseInsensitive);
                    setNode->set.Set(ctAllocator, prevChar->cs[0]) ;
                    setNode->set.UnionInPlace(ctAllocator, currSet->set);
                    return setNode;
                }
            }
            else
                // Can't merge
                return 0;
        }
        else if (prev->tag == Node::MatchSet)
        {
            MatchSetNode* prevSet = (MatchSetNode*)prev;
            if (prevSet->isNegation)
                // Can't merge
                return 0;
            else if (curr->tag == Node::MatchChar)
            {
                MatchCharNode* currChar = (MatchCharNode*)curr;
                // Include char in prev set
                prevSet->set.Set(ctAllocator, currChar->cs[0]);
                return prevSet;
            }
            else if (curr->tag == Node::MatchSet)
            {
                MatchSetNode* currSet = (MatchSetNode*)curr;
                if (currSet->isNegation)
                    // Can't merge
                    return 0;
                else
                {
                    // Include chars in prev set
                    prevSet->set.UnionInPlace(ctAllocator, currSet->set);
                    return prevSet;
                }
            }
            else
                // Can't merge
                return 0;
        }
        else
            // Can't merge
            return 0;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::DisjunctionPass0(int depth)
    {
        PushAltScope();
        AlternativePass0(depth);
        while (true)
        {
            // Could be terminating 0
            if (ECLookahead() != '|')
            {
                PopAltScope();
                return;
            }
            ECConsume();
            FoldAlternativeIntoScope();
            AlternativePass0(depth);
        }
    }

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::DisjunctionPass1()
    {
        // Maintain the invariants:
        //  - alt lists have two or more items
        //  - alt list items are never alt lists (so we must inline them)
        //  - an alt list never contains two consecutive match-character/match-set nodes
        //    (so we must union consecutive items into a single set)
        Node* node = AlternativePass1();
        AltNode* last = 0;
        // First node may be an alternative
        if (node->tag == Node::Alt)
        {
            last = (AltNode*)node;
            while (last->tail != 0)
                last = last->tail;
        }
        while (true)
        {
            // Could be terminating 0
            if (ECLookahead() != '|')
                return node;
            ECConsume(); // '|'
            Node* next = AlternativePass1();
            AnalysisAssert(next != nullptr);
            Node* revisedPrev = UnionNodes(last == 0 ? node : last->head, next);
            if (revisedPrev != 0)
            {
                // Can merge next into previously seen alternative
                if (last == 0)
                    node = revisedPrev;
                else
                    last->head = revisedPrev;
            }
            else if (next->tag == Node::Alt)
            {
                AltNode* nextList = (AltNode*)next;
                // Since we just had to dereference next to get here, we know that nextList
                // can't be nullptr in this case.
                AnalysisAssert(nextList != nullptr);
                // Append inner list to current list
                revisedPrev = UnionNodes(last == 0 ? node : last->head, nextList->head);
                if (revisedPrev != 0)
                {
                    // Can merge head of list into previously seen alternative
                    if (last ==0)
                        node = revisedPrev;
                    else
                        last->head = revisedPrev;
                    nextList = nextList->tail;
                }
                if (last == 0)
                    node = Anew(ctAllocator, AltNode, node, nextList);
                else
                    last->tail = nextList;
                AnalysisAssert(nextList != nullptr);
                while (nextList->tail != 0)
                    nextList = nextList->tail;
                last = nextList;
            }
            else
            {
                // Append node
                AltNode* cons = Anew(ctAllocator, AltNode, next, 0);
                if (last == 0)
                    node = Anew(ctAllocator, AltNode, node, cons);
                else
                    last->tail = cons;
                last = cons;
            }
        }
    }

    template <typename P, const bool IsLiteral>
    inline bool Parser<P, IsLiteral>::IsEndOfAlternative()
    {
        EncodedChar ec = ECLookahead();
        // Could be terminating 0, but embedded 0 is part of alternative
        return (ec == 0 && IsEOF()) || ec == ')' || ec == '|' || (IsLiteral && ec == '/');
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::EnsureLitbuf(CharCount size)
    {
        if (litbufLen - litbufNext < size)
        {
            CharCount newLen = max(litbufLen, initLitbufSize);
            while (newLen < litbufNext + size)
                newLen *= 2;
            litbuf = (Char*)ctAllocator->Realloc(litbuf, litbufLen * sizeof(Char), newLen * sizeof(Char));
            litbufLen = newLen;
        }
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::AccumLiteral(MatchLiteralNode* deferredLiteralNode, Node* charOrLiteralNode)
    {
        Assert(charOrLiteralNode->tag == Node::MatchChar || charOrLiteralNode->tag == Node::MatchLiteral);
        CharCount addLen = charOrLiteralNode->LiteralLength();
        Assert(addLen > 0);

        // Canonical v-flag \q string members compare through per-code-point
        // Canonicalize (MatchLiteralCanonicalInst); they must never merge
        // with an ordinary literal run (whose bytes are raw and whose
        // matcher is exact/equiv), so a mismatch abandons the current run.
        bool incomingCanonical =
            charOrLiteralNode->tag == Node::MatchLiteral && ((MatchLiteralNode*)charOrLiteralNode)->isCanonical;

        if (deferredLiteralNode->length == 0)
        {
            // Start a new literal
            EnsureLitbuf(addLen);
            deferredLiteralNode->offset = litbufNext;
            deferredLiteralNode->length = addLen;
            deferredLiteralNode->isCanonical = incomingCanonical;
            charOrLiteralNode->AppendLiteral(litbufNext, litbufLen, litbuf);
        }
        else if (deferredLiteralNode->isCanonical != incomingCanonical)
        {
            // Abandon current literal and start a fresh one (leaves gap)
            EnsureLitbuf(deferredLiteralNode->length + addLen);
            js_wmemcpy_s(litbuf + litbufNext, litbufLen - litbufNext, litbuf + deferredLiteralNode->offset, deferredLiteralNode->length);
            deferredLiteralNode->offset = litbufNext;
            litbufNext += deferredLiteralNode->length;
            charOrLiteralNode->AppendLiteral(litbufNext, litbufLen, litbuf);
            deferredLiteralNode->length += addLen;
            deferredLiteralNode->isCanonical = incomingCanonical;
        }
        else if (deferredLiteralNode->offset + deferredLiteralNode->length == litbufNext)
        {
            // Keep growing the current literal
            EnsureLitbuf(addLen);
            charOrLiteralNode->AppendLiteral(litbufNext, litbufLen, litbuf);
            deferredLiteralNode->length += addLen;

        }
        else if (charOrLiteralNode->tag == Node::MatchLiteral && deferredLiteralNode->offset + deferredLiteralNode->length == ((MatchLiteralNode*)charOrLiteralNode)->offset)
        {
            // Absorb next literal into current literal since they are adjacent
            deferredLiteralNode->length += addLen;
        }
        else
        {
            // Abandon current literal and start a fresh one (leaves gap)
            EnsureLitbuf(deferredLiteralNode->length + addLen);
            js_wmemcpy_s(litbuf + litbufNext, litbufLen - litbufNext, litbuf + deferredLiteralNode->offset, deferredLiteralNode->length);
            deferredLiteralNode->offset = litbufNext;
            litbufNext += deferredLiteralNode->length;
            charOrLiteralNode->AppendLiteral(litbufNext, litbufLen, litbuf);
            deferredLiteralNode->length += addLen;
        }
    }

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::FinalTerm(Node* node, MatchLiteralNode* deferredLiteralNode)
    {
        if (node == deferredLiteralNode)
        {
#if DBG
            if (deferredLiteralNode->length == 0)
                Assert(false);
#endif
            Assert(deferredLiteralNode->offset < litbufNext);
            Assert(deferredLiteralNode->offset + deferredLiteralNode->length <= litbufNext);
            deferredLiteralNode->isCaseInsensitive = caseInsensitiveFlagPresent;
            if (deferredLiteralNode->length == 1)
            {
                node = Anew(ctAllocator, MatchCharNode, litbuf[deferredLiteralNode->offset], caseInsensitiveFlagPresent);
                if (deferredLiteralNode->offset + deferredLiteralNode->length == litbufNext)
                    // Reclaim last added character
                    litbufNext--;
                // else: leave a gap in the literal buffer
            }
            else
                node = Anew(ctAllocator, MatchLiteralNode, *deferredLiteralNode);
            deferredLiteralNode->offset = 0;
            deferredLiteralNode->length = 0;
        }
        return node;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::AlternativePass0(int depth)
    {
        while (!IsEndOfAlternative())
            TermPass0(depth);
    }

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::AlternativePass1()
    {
        if (IsEndOfAlternative())
            return Anew(ctAllocator, SimpleNode, Node::Empty);

        MatchCharNode deferredCharNode(0, caseInsensitiveFlagPresent);
        MatchLiteralNode deferredLiteralNode(0, 0, caseInsensitiveFlagPresent);

        // Maintain the invariants:
        //  - concat lists have two or more items
        //  - concat list items are never concat lists
        //  - a concat list never contains two consecutive match-character/match-literal nodes
        bool previousSurrogatePart = false;
        Node* node = TermPass1(&deferredCharNode, previousSurrogatePart);
        AnalysisAssert(node != nullptr);
        ConcatNode* last = 0;
        // First node may be a concat
        if (node->tag == Node::Concat)
        {
            last = (ConcatNode*)node;
            while (last->tail != 0)
                last = last->tail;
        }

        if (last == 0)
        {
            if (node->LiteralLength() > 0)
            {
                // Begin a new literal
                AccumLiteral(&deferredLiteralNode, node);
                node = &deferredLiteralNode;
            }
        }
        else
        {
            if (last->head->LiteralLength() > 0)
            {
                // Begin a new literal
                AccumLiteral(&deferredLiteralNode, last->head);
                last->head = &deferredLiteralNode;
            }
        }

        while (!IsEndOfAlternative())
        {
            Node* next = TermPass1(&deferredCharNode, previousSurrogatePart);
            AnalysisAssert(next != nullptr);
            if (next->LiteralLength() > 0)
            {
                // Begin a new literal or grow the existing literal
                AccumLiteral(&deferredLiteralNode, next);
                if (last == 0)
                {
                    if (node != &deferredLiteralNode)
                    {
                        // So far we have first item and the current literal
                        ConcatNode* cons = Anew(ctAllocator, ConcatNode, &deferredLiteralNode, 0);
                        node = Anew(ctAllocator, ConcatNode, node, cons);
                        last = cons;
                    }
                    // else: keep growing first literal
                }
                else
                {
                    if (last->head != &deferredLiteralNode)
                    {
                        // Append a new literal node
                        ConcatNode* cons = Anew(ctAllocator, ConcatNode, &deferredLiteralNode, 0);
                        last->tail = cons;
                        last = cons;
                    }
                    // else: keep growing current literal
                }
            }
            else if (next->tag == Node::Concat)
            {
                // Append this list to accumulated list
                ConcatNode* nextList = (ConcatNode*)next;
                if (nextList->head->LiteralLength() > 0 &&
                    ((last == 0 && node == &deferredLiteralNode) ||
                     (last != 0 && last->head == &deferredLiteralNode)))
                {
                    // Absorb the next character or literal into the current literal
                    // (may leave a gab in litbuf)
                    AccumLiteral(&deferredLiteralNode, nextList->head);
                    nextList = nextList->tail;
                    // List has at least two items
                    AnalysisAssert(nextList != 0);
                    // List should be in canonical form, so no consecutive chars/literals
                    Assert(nextList->head->LiteralLength() == 0);
                }
                if (last == 0)
                    node = Anew(ctAllocator, ConcatNode, FinalTerm(node, &deferredLiteralNode), nextList);
                else
                {
                    last->head = FinalTerm(last->head, &deferredLiteralNode);
                    last->tail = nextList;
                }
                // NextList can't be nullptr, since it was last set as next, which
                // was dereferenced, or it was set on a path that already has this
                // analysis assert.
                AnalysisAssert(nextList != nullptr);
                while (nextList->tail != 0)
                    nextList = nextList->tail;
                last = nextList;
                // No outstanding literals
                Assert(deferredLiteralNode.length == 0);
                // We just set this from nextList, which we know is not nullptr.
                AnalysisAssert(last != nullptr);
                if (last->head->LiteralLength() > 0)
                {
                    // If the list ends with a literal, transfer it into deferredLiteralNode
                    // so we can continue accumulating (won't leave a gab in litbuf)
                    AccumLiteral(&deferredLiteralNode, last->head);
                    // Can discard MatchLiteralNode since it lives in compile-time allocator
                    last->head = &deferredLiteralNode;
                }
            }
            else
            {
                // Append this node to accumulated list
                ConcatNode* cons = Anew(ctAllocator, ConcatNode, next, 0);
                if (last == 0)
                    node = Anew(ctAllocator, ConcatNode, FinalTerm(node, &deferredLiteralNode), cons);
                else
                {
                    last->head = FinalTerm(last->head, &deferredLiteralNode);
                    last->tail = cons;
                }
                last = cons;
                // No outstanding literals
                Assert(deferredLiteralNode.length == 0);
            }
        }
        if (last == 0)
            node = FinalTerm(node, &deferredLiteralNode);
        else
            last->head = FinalTerm(last->head, &deferredLiteralNode);
        // No outstanding literals
        Assert(deferredLiteralNode.length == 0);

        return node;
    }

    //
    // Terms
    //

    // ----------------------------------------------------------------------
    // Lookbehind body mirroring (see the declaration in RegexParser.h)
    // ----------------------------------------------------------------------

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::MirrorLiteral(const MatchLiteralNode* lit)
    {
        Assert(!lit->isEquivClass); // equiv classes are a compile-time expansion; parse literals are plain
        MatchLiteralNode* reversed = Anew(ctAllocator, MatchLiteralNode, 0, 0, lit->isCaseInsensitive);
        if (unicodeFlagPresent)
        {
            // u-mode: the runtime window is reversed by code points (see
            // BeginLookbehindInst::Exec), so mirror the literal the same
            // way — reverse the code points but keep a surrogate pair's two
            // units in order. (Astral pattern characters are two-unit
            // literals here; lone surrogates are one-unit code points.)
            CharCount i = lit->length;
            while (i > 0)
            {
                const Char c = litbuf[lit->offset + i - 1];
                if (Js::NumberUtilities::IsSurrogateUpperPart(c) && i >= 2
                    && Js::NumberUtilities::IsSurrogateLowerPart(litbuf[lit->offset + i - 2]))
                {
                    MatchCharNode lower(litbuf[lit->offset + i - 2], lit->isCaseInsensitive);
                    MatchCharNode upper(c, lit->isCaseInsensitive);
                    AccumLiteral(reversed, &lower);
                    AccumLiteral(reversed, &upper);
                    i -= 2;
                }
                else
                {
                    MatchCharNode cNode(c, lit->isCaseInsensitive);
                    AccumLiteral(reversed, &cNode);
                    i -= 1;
                }
            }
        }
        else
        {
            for (CharCount i = lit->length; i > 0; i--)
            {
                MatchCharNode c(litbuf[lit->offset + i - 1], lit->isCaseInsensitive);
                AccumLiteral(reversed, &c);
            }
        }
        Assert(reversed->length == lit->length);
        return reversed;
    }

    // True for the parser's synthetic surrogate-pair atoms: a concat of
    // exactly two terms, the first a lead-surrogate atom (a lead char or a
    // set contained in [D800-DBFF]) and the second a trail-surrogate-only
    // set ([DC00-DFFF] subrange). These are built by
    // CreateSurrogatePairAtom (astral singletons in classes/escapes) and
    // GetNodeWithValidCharacterSet (u-mode . / \S / \D / \W); the two units
    // are ONE code point, so in u-mode (whose reversed window keeps pairs in
    // unit order) mirroring must preserve their order.
    template <typename P, const bool IsLiteral>
    bool Parser<P, IsLiteral>::IsSurrogatePairConcat(Node* node)
    {
        if (node->tag != Node::Concat)
        {
            return false;
        }
        ConcatNode* const c1 = (ConcatNode*)node;
        if (c1->tail == nullptr || c1->tail->tag != Node::Concat)
        {
            return false;
        }
        ConcatNode* const c2 = (ConcatNode*)c1->tail;
        if (c2->tail != nullptr)
        {
            // The pair atoms are always spliced in as a whole alternation
            // term, never extended with more terms.
            return false;
        }

        auto setIsSingleRangeWithin = [](CharSet<Char>& set, Char lowerBound, Char upperBound) {
            Char rangeLower, rangeUpper;
            if (!set.GetNextRange(0, &rangeLower, &rangeUpper))
            {
                return false;
            }
            Char nextLower, nextUpper;
            if (set.GetNextRange(rangeUpper == Char(0xFFFFu) ? Char(0xFFFFu) : Char(rangeUpper + 1), &nextLower, &nextUpper))
            {
                return false;
            }
            return rangeLower >= lowerBound && rangeUpper <= upperBound;
        };

        auto isLeadAtom = [&](Node* n) {
            if (n->tag == Node::MatchChar)
            {
                const Char ch = ((MatchCharNode*)n)->cs[0];
                return ch >= (Char)0xD800u && ch <= (Char)0xDBFFu;
            }
            if (n->tag == Node::MatchSet)
            {
                return setIsSingleRangeWithin(((MatchSetNode*)n)->set, (Char)0xD800u, (Char)0xDBFFu);
            }
            return false;
        };

        return isLeadAtom(c1->head)
            && c2->head->tag == Node::MatchSet
            && setIsSingleRangeWithin(((MatchSetNode*)c2->head)->set, (Char)0xDC00u, (Char)0xDFFFu);
    }

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::MirrorLookbehindBody(Node* body)
    {
        PROBE_STACK_NO_DISPOSE(scriptContext, Js::Constants::MinStackRegex);

        switch (body->tag)
        {
        case Node::Concat:
            {
                // Reverse the term order, mirroring each term. The list's
                // canonical form (no two consecutive char/literal nodes) is
                // preserved by reversal. Exception: in u-mode a synthetic
                // surrogate-pair atom's two units are one code point, which
                // the code-point-reversed window presents in original unit
                // order, so such a two-term list is NOT reversed.
                ConcatNode* prev = nullptr;
                if (!(unicodeFlagPresent && IsSurrogatePairConcat(body)))
                {
                    for (ConcatNode* cur = (ConcatNode*)body; cur != nullptr;)
                    {
                        ConcatNode* const next = cur->tail;
                        cur->tail = prev;
                        prev = cur;
                        cur = next;
                    }
                }
                else
                {
                    prev = (ConcatNode*)body;
                }
                for (ConcatNode* p = prev; p != nullptr; p = p->tail)
                {
                    p->head = MirrorLookbehindBody(p->head);
                }
                return prev;
            }
        case Node::Alt:
            {
                // Alternatives keep textual order (the spec's reverse matcher
                // tries them in textual order); each is mirrored.
                for (AltNode* alt = (AltNode*)body; alt != nullptr; alt = alt->tail)
                {
                    alt->head = MirrorLookbehindBody(alt->head);
                }
                return body;
            }
        case Node::Loop:
            {
                // Greedy/lazy preference and iteration counts are unchanged;
                // greediness now extends leftward because the whole body runs
                // over the reversed window.
                LoopNode* loop = (LoopNode*)body;
                loop->body = MirrorLookbehindBody(loop->body);
                return loop;
            }
        case Node::DefineGroup:
            {
                DefineGroupNode* group = (DefineGroupNode*)body;
                group->body = MirrorLookbehindBody(group->body);
                return group;
            }
        case Node::Modifier:
            {
                // The scoped-modifier wrapper is transparent; mirror its
                // body so characters inside the modified body reverse too.
                ModifierNode* modifier = (ModifierNode*)body;
                modifier->body = MirrorLookbehindBody(modifier->body);
                return modifier;
            }
        case Node::Assertion:
            {
                // A lookahead inside the lookbehind: at runtime it must look
                // forward over the real input, which a suffix-frame
                // LookbehindNode provides. Its body must NOT be re-mirrored
                // (any lookbehind inside it already baked its own mirrored
                // body at its close).
                AssertionNode* assertion = (AssertionNode*)body;
                return Anew(ctAllocator, LookbehindNode, assertion->isNegation, assertion->body,
                    /*isSuffixFrame=*/true);
            }
        case Node::Lookbehind:
            {
                // A source lookbehind inside this body: matching its (already
                // mirrored) stored body forward over the current frame from
                // the current position is exactly its reverse match over the
                // real text, i.e. a plain lookahead here.
                LookbehindNode* lookbehind = (LookbehindNode*)body;
                Assert(!lookbehind->isSuffixFrame);
                return Anew(ctAllocator, AssertionNode, lookbehind->isNegation, lookbehind->body);
            }
        case Node::MatchLiteral:
            // Literal characters are reversed with the term order. In u-mode
            // both the runtime window and the mirrored literal are reversed
            // by code points (surrogate pairs stay intact); without u the
            // reversal is unit-wise, which is what the unit-reversed window
            // contains.
            return MirrorLiteral((MatchLiteralNode*)body);
        default:
            // MatchChar/MatchSet/classes/dots, numbered and named backrefs,
            // and (frame-aware) anchors and word boundaries are
            // direction-symmetric once the frame machinery orients the
            // window.
            return body;
        }
    }

    template <typename P, const bool IsLiteral>
    bool Parser<P, IsLiteral>::TryParseModifierGroup(RegexFlags& addFlags, RegexFlags& removeFlags)
    {
        // next points at the '?' that starts the group; the callers have
        // already consumed the '('.
        Assert(ECLookahead() == '?');
        const EncodedChar first = ECLookahead(1);
        if (first != 'i' && first != 'm' && first != 's' && first != '-')
            return false;

        // This is an ES2025 modifier group (or a malformed one): after the
        // first flag character or '-', the grammar admits only i/m/s, one
        // optional '-' and a terminating ':'. Anything else is a
        // SyntaxError, never an ordinary group.
        ECConsume(); // ?
        auto toFlag = [](EncodedChar ec) -> RegexFlags {
            switch (Chars<EncodedChar>::CTW(ec))
            {
            case 'i': return IgnoreCaseRegexFlag;
            case 'm': return MultilineRegexFlag;
            case 's': return DotAllRegexFlag;
            default: return NoRegexFlags;
            }
        };

        addFlags = NoRegexFlags;
        removeFlags = NoRegexFlags;

        while (true)
        {
            const RegexFlags flag = toFlag(ECLookahead());
            if (flag == NoRegexFlags)
                break;
            if ((addFlags & flag) != 0)
            {
                Fail(JSERR_RegExpSyntax);
                return false;
            }
            addFlags = (RegexFlags)(addFlags | flag);
            ECConsume();
        }

        if (ECLookahead() == '-')
        {
            ECConsume();
            while (true)
            {
                const RegexFlags flag = toFlag(ECLookahead());
                if (flag == NoRegexFlags)
                    break;
                // A flag may appear in only one of the two sets.
                if (((addFlags | removeFlags) & flag) != 0)
                {
                    Fail(JSERR_RegExpSyntax);
                    return false;
                }
                removeFlags = (RegexFlags)(removeFlags | flag);
                ECConsume();
            }
        }

        // The arithmetic form requires at least one flag in total, and every
        // form requires the ':' that starts the modified disjunction.
        if ((addFlags | removeFlags) == NoRegexFlags || ECLookahead() != ':')
        {
            Fail(JSERR_RegExpSyntax);
            return false;
        }
        ECConsume();

        this->hasScopedModifiers = true;
        return true;
    }

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::NewLoopNode(CharCount lower, CharCountOrFlag upper, bool isGreedy, Node* body)
    {
        //
        // NOTE: We'd like to represent r? (i.e. r{0,1}) as r|<empty> since the loop representation has high overhead.
        //       HOWEVER if r contains a group definition and could match empty then we must execute as a loop
        //       so that group bindings are correctly reset on no progress (e.g.: /(a*)?/.exec("")). Thus we defer
        //       this optimization until pass 4 of the optimizer, at which point we know whether r could match empty.
        //
        if (lower == 1 && upper == 1)
            return body;
        else if (lower == 0 && upper == 0)
            // Loop is equivalent to empty. If the loop body contains group definitions they will have already been
            // counted towards the overall number of groups. The matcher will initialize their contents to
            // undefined, and since the loop body would never execute the inner groups could never be updated from
            // undefined.
            return Anew(ctAllocator, SimpleNode, Node::Empty);
        else
            return Anew(ctAllocator, LoopNode, lower, upper, isGreedy, body);
    }

    template <typename P, const bool IsLiteral>
    bool Parser<P, IsLiteral>::AtQuantifier()
    {
        // Could be terminating 0
        switch (ECLookahead())
        {
        case '*':
        case '+':
        case '?':
            return true;
        case '{':
            {
                CharCount lookahead = 1;
                while (ECCanConsume(lookahead + 1) && standardEncodedChars->IsDigit(ECLookahead(lookahead)))
                    lookahead++;
                if (lookahead == 1 || !ECCanConsume(lookahead + 1))
                    return false;
                switch (ECLookahead(lookahead))
                {
                case ',':
                    lookahead++;
                    if (ECCanConsume(lookahead + 1) && ECLookahead(lookahead) == '}')
                        return true;
                    else
                    {
                        CharCount saved = lookahead;
                        while (ECCanConsume(lookahead + 1) && standardEncodedChars->IsDigit(ECLookahead(lookahead)))
                            lookahead++;
                        if (lookahead == saved)
                            return false;
                        return ECCanConsume(lookahead + 1) && ECLookahead(lookahead) == '}';
                    }
                case '}':
                    return true;
                default:
                    return false;
                }
            }
        default:
            return false;
        }
    }

    template <typename P, const bool IsLiteral>
    bool Parser<P, IsLiteral>::OptNonGreedy()
    {
        // Could be terminating 0
        if (ECLookahead() != '?')
            return true;
        ECConsume();
        return false;
    }

    template <typename P, const bool IsLiteral>
    CharCount Parser<P, IsLiteral>::RepeatCount()
    {
        CharCount n = 0;
        int digits = 0;
        while (true)
        {
            // Could be terminating 0
            EncodedChar ec = ECLookahead();
            if (!standardEncodedChars->IsDigit(ec))
            {
                if (digits == 0)
                {
                    Fail(JSERR_RegExpSyntax);
                }

                return n;
            }

            if (n > MaxCharCount / 10)
            {
                break;
            }

            n *= 10;
            if (n > MaxCharCount - standardEncodedChars->DigitValue(ec))
            {
                break;
            }

            n += standardEncodedChars->DigitValue(ec);
            digits++;
            ECConsume();
        }

        Assert(digits != 0); // shouldn't be able to reach here with (digits == 0)

        // The token had a value larger than MaxCharCount so we didn't return the value and reached here instead.
        // Consume the rest of the token and return MaxCharCount.
        while (true)
        {
            EncodedChar ec = ECLookahead();
            if (!standardEncodedChars->IsDigit(ec))
            {
                return MaxCharCount;
            }
            ECConsume();
        }
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::TermPass0(int depth)
    {
        PROBE_STACK_NO_DISPOSE(scriptContext, Js::Constants::MinStackRegex);
        // Either we have a location at the start, or the end, never both. As in between it should have been cleared if surrogate pair
        // Or must be cleared if we didn't perform the check
        bool clearLocationIfPresent = this->tempLocationOfSurrogatePair != nullptr;
        // Set when this term is a (?<= / (?<! lookbehind assertion: those are
        // never quantifiable (ES2018 removed the Annex B QuantifiableAssertion
        // relaxation for them), in either mode.
        bool thisTermIsLookbehind = false;
        // Set when this term is a (?= / (?! lookahead assertion: Annex B
        // allows a quantifier on it (QuantifiableAssertion), u/v modes do
        // not, so the quantifier records a deferred-if-unicode error.
        bool thisTermIsLookahead = false;

        switch (ECLookahead())
        {
        case '^':
        case '$':
            ECConsume();
            return;
        case '\\':
            ECConsume();
            if (AtomEscapePass0())
                return;
            break;
        case '(':
            // Can't combine into a single codeunit because of group present
            this->tempLocationOfSurrogatePair = nullptr;
            this->codePointAtTempLocation = 0;
            clearLocationIfPresent = false;
            ECConsume();
            switch (ECLookahead())
            {
            case '?':
                if (!ECCanConsume(2))
                    Fail(JSERR_RegExpSyntax);
                switch (ECLookahead(1))
                {
                case '=':
                case '!':
                    ECConsume(2);
                    thisTermIsLookahead = true;
                    break;
                case ':':
                    ECConsume(2);
                    break;
                case '<':
                    {
                        // (?<Name>...) named capture group, or (?<= / (?<!)
                        // lookbehind assertions (ES2018). An unparsable name
                        // after (?< (e.g. (?< followed by EOF or another
                        // invalid start) is still rejected below by the
                        // ID_Start check in ScanGroupName.
                        if (ECCanConsume(3) && (ECLookahead(2) == '=' || ECLookahead(2) == '!'))
                        {
                            // Lookbehind assertion: non-capturing, and never
                            // quantifiable (unlike Annex B lookaheads).
                            ECConsume(3);
                            thisTermIsLookbehind = true;
                            break;
                        }
                        if (!ECCanConsume(3))
                            Fail(JSERR_RegExpSyntax);
                        ECConsume(2); // ?<
                        CharCount nameOffset = this->namedGroupBufferNext;
                        ScanGroupName();
                        RecordNamedGroup(this->namedGroupBuffer + nameOffset,
                            this->namedGroupBufferNext - nameOffset, numGroups);
                        numGroups++;
                    }
                    break;
                default:
                    {
                        // ES2025 scoped modifiers: (?ims-ims:Disjunction).
                        // Non-capturing, so no group number is consumed; the
                        // common code below parses the body and ')'.
                        RegexFlags addFlags;
                        RegexFlags removeFlags;
                        if (!TryParseModifierGroup(addFlags, removeFlags))
                        {
                            numGroups++;
                        }
                    }
                    break;
                }
                break;
            default:
                numGroups++;
                break;
            }
            DisjunctionPass0(depth + 1);
            // A surrogate at the end of a group cannot combine with a low
            // surrogate after the group; clear the term-level tracker before
            // consuming the close so pass 1 sees the real group boundary.
            this->tempLocationOfSurrogatePair = nullptr;
            this->codePointAtTempLocation = 0;
            ECMust(')', JSERR_RegExpNoParen);
            break;
        case '.':
            ECConsume();
            break;
        case '[':
            ECConsume();
            // v flag (ES2024 Unicode sets): validate the extended class
            // grammar ahead of the legacy scan. This only records
            // deferred-if-v errors (the same text is often a legal u/Annex B
            // class) and restores the cursor, so the legacy scan below and
            // the non-v re-parse in pass 1 see the same input.
            if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
            {
                VClassPass0();
            }
            this->tempLocationOfSurrogatePair = nullptr;
            this->codePointAtTempLocation = 0;
            this->tempLocationOfRange = next;
            CharacterClassPass0();
            this->tempLocationOfRange = nullptr;
            ECMust(']', JSERR_RegExpNoBracket);
            break;
        case ')':
        case '|':
            Fail(JSERR_RegExpSyntax);
            break;
        case ']':
            // v mode (like every u-mode grammar) has no bare ']' atom
            // outside a class. The legacy scanner ends nested/\q classes
            // early, so a ']' the legacy scan treats as an atom is genuine
            // v class content exactly when it lies inside the span the v
            // pass-0 validator consumed for the most recent class
            // ([a[b]c], [[ab][cd]], [\q{a|b}]); outside that span
            // ([]a], [a]], [[a]]|]) it is an atom and rejects under v.
            // Inside the span the content is v grammar: legal under v and
            // under non-u Annex B, but plain u has no nested classes, so it
            // rejects there (deferred-if-unicode-without-v). Non-v modes
            // keep the Annex B leniency exactly as before.
            if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
            {
                bool inSpan = this->vClassSpanStart != nullptr && next >= this->vClassSpanStart && next < this->vClassSpanEnd;
                if (!inSpan)
                {
                    DeferredFailIfUnicodeSets(JSERR_RegExpSyntax);
                    DeferredFailIfUnicode(JSERR_RegExpSyntax);
                }
                else
                {
                    DeferredFailIfUnicodeNoSets(JSERR_RegExpSyntax);
                }
            }
            NextChar();
            break;
        case '}':
            // '}' is not a PatternCharacter under u (only Annex B has the
            // ExtendedPatternCharacter literal fallback for it), except
            // inside a v class span the legacy scanner split early (the
            // closing braces of \q{...} bodies and nested-class content),
            // where it is v grammar: legal under v, rejected under plain u.
            if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
            {
                if (this->vClassSpanStart != nullptr && next >= this->vClassSpanStart && next < this->vClassSpanEnd)
                {
                    DeferredFailIfUnicodeNoSets(JSERR_RegExpSyntax);
                }
                else
                {
                    DeferredFailIfUnicode(JSERR_RegExpSyntax);
                }
            }
            NextChar();
            break;
        case '*':
        case '+':
        case '?':
        case '{':
            if (AtQuantifier())
                Fail(JSERR_RegExpBadQuant);
            else
            {
                if (ECLookahead() == '{' && scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                {
                    if (this->vClassSpanStart != nullptr && next >= this->vClassSpanStart && next < this->vClassSpanEnd)
                    {
                        // v class-span content (see '}' above): legal under
                        // v, rejected under plain u, literal without u.
                        DeferredFailIfUnicodeNoSets(JSERR_RegExpSyntax);
                    }
                    else
                    {
                        // A '{' that does not start a quantifier is an Annex B
                        // literal only; u mode has no ExtendedPatternCharacter
                        // fallback for it.
                        DeferredFailIfUnicode(JSERR_RegExpSyntax);
                    }
                }
                ECConsume();
            }
            break;
        case 0:
            if (IsEOF())
                // Terminating 0
                Fail(JSERR_RegExpSyntax);
            // else fall-through for embedded 0
        default:
            {
                const EncodedChar* current = next;
                Char c = NextChar();
                if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                {
                    TrackIfSurrogatePair(c, current, (uint32)(next - current));
                }
                // Closing '/' in literals should be caught explicitly
                Assert(!IsLiteral || c != '/');
            }
            break;
        }

        if (clearLocationIfPresent && this->tempLocationOfSurrogatePair != nullptr)
        {
            this->tempLocationOfSurrogatePair = nullptr;
            this->codePointAtTempLocation = 0;
        }

        if (AtQuantifier())
        {
            if (thisTermIsLookbehind)
                Fail(JSERR_RegExpBadQuant);
            if (thisTermIsLookahead && scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
            {
                // Annex B QuantifiableAssertion+Quantifier; u/v modes reject
                // (deferred so the non-u reading is unchanged).
                DeferredFailIfUnicode(JSERR_RegExpBadQuant);
            }
            switch (ECLookahead())
            {
            case '*':
            case '+':
            case '?':
                ECConsume();
                OptNonGreedy();
                break;
            case '{':
                {
                    ECConsume();
                    CharCount lower = RepeatCount();
                    switch (ECLookahead())
                    {
                    case ',':
                        ECConsume();
                        if (ECLookahead() == '}')
                        {
                            ECConsume();
                            OptNonGreedy();
                        }
                        else
                        {
                            CharCount upper = RepeatCount();
                            if (upper < lower)
                                Fail(JSERR_RegExpSyntax);
                            Assert(ECLookahead() == '}');
                            ECConsume();
                            OptNonGreedy();
                        }
                        break;
                    case '}':
                        ECConsume();
                        OptNonGreedy();
                        break;
                    default:
                        Assert(false);
                        break;
                    }
                    break;
                }
            default:
                Assert(false);
                break;
            }
        }
    }

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::TermPass1(MatchCharNode* deferredCharNode, bool& previousSurrogatePart)
    {
        PROBE_STACK_NO_DISPOSE(scriptContext, Js::Constants::MinStackRegex);

        Node* node = 0;
        bool containsSurrogatePair = false;
        switch (ECLookahead())
        {
        case '^':
            ECConsume();
            // No quantifier allowed. Inside a lookbehind the anchor must be
            // tested against the real input through the current frame.
            return Anew(ctAllocator, SimpleNode, Node::BOL, lookbehindDepth > 0, multilineFlagPresent);
        case '$':
            ECConsume();
            return Anew(ctAllocator, SimpleNode, Node::EOL, lookbehindDepth > 0, multilineFlagPresent); // No quantifier allowed
        case '\\':
            ECConsume();
            if(SurrogatePairPass1(node, deferredCharNode, previousSurrogatePart))
            {
                break; // For quantifier
            }
            else if (AtomEscapePass1(node, deferredCharNode, previousSurrogatePart))
            {
                return node; // No quantifier allowed
            }
            break; // else: fall-through for opt quantifier
        case '(':
            ECConsume();
            switch (ECLookahead())
            {
            case '?':
                switch (ECLookahead(1))
                {
                case '=':
                    ECConsume(2); // ?=
                    {
                        // Inside a lookbehind the lookahead body also runs in
                        // a non-top frame (the real suffix), so anchors within
                        // it must stay frame-aware.
                        if (lookbehindDepth > 0)
                            lookbehindDepth++;
                        node = DisjunctionPass1();
                        if (lookbehindDepth > 0)
                            lookbehindDepth--;
                        Assert(ECLookahead() == ')');
                        ECConsume(); // )
                        node = Anew(ctAllocator, AssertionNode, false, node);
                    }
                    break; // As per Annex B, allow this to be quantifiable
                case '!':
                    ECConsume(2); // ?!
                    {
                        if (lookbehindDepth > 0)
                            lookbehindDepth++;
                        node = DisjunctionPass1();
                        if (lookbehindDepth > 0)
                            lookbehindDepth--;
                        Assert(ECLookahead() == ')');
                        ECConsume(); // )
                        node = Anew(ctAllocator, AssertionNode, true, node);
                    }
                    break; // As per Annex B, allow this to be quantifiable
                case ':':
                    ECConsume(2); // ?:
                    node = DisjunctionPass1();
                    Assert(ECLookahead() == ')');
                    ECConsume(); // )
                    break; // fall-through for opt quantifier
                case '<':
                    {
                        if (ECCanConsume(3) && (ECLookahead(2) == '=' || ECLookahead(2) == '!'))
                        {
                            // (?<=e) / (?<!e) lookbehind: parse the body and
                            // mirror it so it can be matched forward over the
                            // reversed left context. Never quantifiable (pass
                            // 0 rejects the quantifier, so none can reach
                            // here).
                            const bool isNegation = ECLookahead(2) == '!';
                            ECConsume(3); // ?<= or (?<!
                            lookbehindDepth++;
                            node = DisjunctionPass1();
                            lookbehindDepth--;
                            Assert(ECLookahead() == ')');
                            ECConsume(); // )
                            node = Anew(ctAllocator, LookbehindNode, isNegation,
                                MirrorLookbehindBody(node), /*isSuffixFrame=*/false);
                            return node; // no quantifier allowed
                        }
                        // (?<Name>...) — name already validated during pass 0
                        // and resolved against the definition registry.
                        ECConsume(2); // ?<
                        ScanGroupName();
                        int thisGroupId = nextGroupId++;
                        node = DisjunctionPass1();
                        Assert(ECLookahead() == ')');
                        ECConsume(); // )
                        node = Anew(ctAllocator, DefineGroupNode, thisGroupId, node);
                        break; // fall-through for opt quantifier
                    }
                default:
                    {
                        RegexFlags addFlags;
                        RegexFlags removeFlags;
                        if (TryParseModifierGroup(addFlags, removeFlags))
                        {
                            // ES2025 scoped modifiers: apply UpdateModifiers
                            // for the body only, then restore. u cannot be
                            // modified; i affects parse-time folding, sets,
                            // escapes and backreference matching while s
                            // selects the dot set and m is recorded on the
                            // ^/$ nodes.
                            const bool savedCaseInsensitive = caseInsensitiveFlagPresent;
                            const bool savedDotAll = dotAllFlagPresent;
                            const bool savedMultiline = multilineFlagPresent;
                            if ((addFlags & IgnoreCaseRegexFlag) != 0)
                                caseInsensitiveFlagPresent = true;
                            if ((addFlags & DotAllRegexFlag) != 0)
                                dotAllFlagPresent = true;
                            if ((addFlags & MultilineRegexFlag) != 0)
                                multilineFlagPresent = true;
                            if ((removeFlags & IgnoreCaseRegexFlag) != 0)
                                caseInsensitiveFlagPresent = false;
                            if ((removeFlags & DotAllRegexFlag) != 0)
                                dotAllFlagPresent = false;
                            if ((removeFlags & MultilineRegexFlag) != 0)
                                multilineFlagPresent = false;

                            node = DisjunctionPass1();
                            Assert(ECLookahead() == ')');
                            ECConsume(); // )
                            node = Anew(ctAllocator, ModifierNode, node);

                            caseInsensitiveFlagPresent = savedCaseInsensitive;
                            dotAllFlagPresent = savedDotAll;
                            multilineFlagPresent = savedMultiline;
                            break; // fall-through for opt quantifier
                        }
                        // ? not yet consumed
                        int thisGroupId = nextGroupId++;
                        node = DisjunctionPass1();
                        Assert(ECLookahead() == ')');
                        ECConsume(); // )
                        node = Anew(ctAllocator, DefineGroupNode, thisGroupId, node);
                        break; // fall-through for opt quantifier
                    }
                }
                break;
            default:
                {
                    // next char not yet consumed
                    int thisGroupId = nextGroupId++;
                    node = DisjunctionPass1();
                    Assert(ECLookahead() == ')');
                    ECConsume(); // )
                    node = Anew(ctAllocator, DefineGroupNode, thisGroupId, node);
                    break; // fall-through for opt quantifier
                }
            }
            break;
        case '.':
            {
                ECConsume();
                node = GetNodeWithValidCharacterSet('.');
                break; // fall-through for opt quantifier
            }
        case '[':
            ECConsume();
            if (unicodeSetsFlagPresent)
            {
                // v flag: the extended class grammar replaces the legacy
                // ClassRanges parse entirely.
                node = VClassPass1();
                Assert(ECLookahead() == ']');
                ECConsume(); // ]
                break; // fall-through for opt quantifier
            }
            if (unicodeFlagPresent)
            {
                containsSurrogatePair = this->currentSurrogatePairNode != nullptr && this->currentSurrogatePairNode->rangeLocation == next;
            }

            node = containsSurrogatePair ? CharacterClassPass1<true>() : CharacterClassPass1<false>();

            Assert(ECLookahead() == ']');
            ECConsume(); // ]
            break; // fall-through for opt quantifier
#if DBG
        case ')':
        case '|':
            Assert(false);
            break;
#endif
        case ']':
            // SPEC DEVIATION: This should be syntax error, instead accept as itself
            deferredCharNode->cs[0] = NextChar();
            node = deferredCharNode;
            break; // fall-through for opt quantifier
#if DBG
        case '*':
        case '+':
        case '?':
            AssertMsg(false, "Allowed only in the escaped form. These should be caught by TermPass0.");
            break;
#endif
        case 0:
            if (IsEOF())
                // Terminating 0
                Fail(JSERR_RegExpSyntax);
            // else fall-through for embedded 0
        default:
            if(SurrogatePairPass1(node, deferredCharNode, previousSurrogatePart))
            {
                break; //For quantifier
            }
            else
            {
                deferredCharNode->cs[0] = NextChar();
                node = deferredCharNode;
                break; // fall-through for opt quantifier
            }
        }

        Assert(node != 0);

        if (AtQuantifier())
        {
            switch (ECLookahead())
            {
            case '*':
                if (node == deferredCharNode)
                    node = Anew(ctAllocator, MatchCharNode, *deferredCharNode);
                ECConsume();
                return NewLoopNode(0, CharCountFlag, OptNonGreedy(), node);
            case '+':
                if (node == deferredCharNode)
                    node = Anew(ctAllocator, MatchCharNode, *deferredCharNode);
                ECConsume();
                return NewLoopNode(1, CharCountFlag, OptNonGreedy(), node);
            case '?':
                if (node == deferredCharNode)
                    node = Anew(ctAllocator, MatchCharNode, *deferredCharNode);
                ECConsume();
                return NewLoopNode(0, 1, OptNonGreedy(), node);
            case '{':
                {
                    if (node == deferredCharNode)
                        node = Anew(ctAllocator, MatchCharNode, *deferredCharNode);
                    ECConsume();
                    CharCount lower = RepeatCount();
                    switch (ECLookahead())
                    {
                    case ',':
                        ECConsume();
                        if (ECLookahead() == '}')
                        {
                            ECConsume();
                            return NewLoopNode(lower, CharCountFlag, OptNonGreedy(), node);
                        }
                        else
                        {
                            CharCount upper = RepeatCount();
                            Assert(lower <= upper);
                            Assert(ECLookahead() == '}');
                            ECConsume(); // }
                            return NewLoopNode(lower, upper, OptNonGreedy(), node);
                        }
                    case '}':
                        ECConsume();
                        return NewLoopNode(lower, lower, OptNonGreedy(), node);
                    default:
                        Assert(false);
                        break;
                    }
                    break;
                }
            default:
                Assert(false);
                break;
            }
        }

        return node;
    }

#pragma warning(push)
#pragma warning(disable:4702)   // unreachable code
    template <typename P, const bool IsLiteral>
    bool Parser<P, IsLiteral>::AtomEscapePass0()
    {
        EncodedChar ec = ECLookahead();
        if (ec == 0 && IsEOF())
        {
            // Terminating 0
            Fail(JSERR_RegExpSyntax);
            return false;
        }
        else if (standardEncodedChars->IsDigit(ec))
        {
            RecordDecimalEscapePass0();
            do
            {
                ECConsume();
            }
            while (standardEncodedChars->IsDigit(ECLookahead())); // terminating 0 is not a digit
            return false;
        }
        else if (ECLookahead() == 'c')
        {
            if (standardEncodedChars->IsLetter(ECLookahead(1))) // terminating 0 is not a letter
            {
                ECConsume(2);
            }
            else
            {
                DeferredFailIfUnicode(JSERR_RegExpInvalidEscape);
            }
            return false;
        }
        else if (ec == 'p' || ec == 'P')
        {
            // \p{...}/\P{...} property escape: consumed and validated here
            // (deferred-if-unicode for the malformed/unknown-name cases);
            // without the u flag pass 1 re-parses it as the identity escape
            // 'p'/'P' plus literal '{...}' characters.
            ScanPropertyEscapePass0();
            return false;
        }
        else if (ec == 'k')
        {
            // \k<Name> named backreference (or Annex B identity escape when
            // the pattern has no named captures and no u flag). Whether the
            // escape is a reference cannot be decided here (forward group
            // definitions), so record the raw span and validate at
            // resolution time; pass 1 re-parses accordingly.
            if (ECCanConsume(2) && ECLookahead(1) == '<')
            {
                ECConsume(2); // k<
                const EncodedChar* nameStart = next;
                // Raw scan to the closing '>'; validation is deferred to
                // ResolveNamedGroupRefs (an Annex B identity escape must not
                // be rejected here). NextChar advances multi-unit characters
                // whole; '>' is ASCII and cannot occur inside a sequence.
                // Structural characters (group/alternative/quantifier
                // syntax) stop the scan as "unterminated": when the escape
                // is a reference the name is invalid anyway, and when it
                // re-parses as an identity escape pass 1 must still see the
                // structure ('\k<a)' must not swallow the ')').
                const EncodedChar* afterGt = nullptr;
                // A backslash escapes the next character. An escaped '/' is
                // not the end of a literal (`/\k<a\/>/` is the identity
                // escape 'k' plus "<a/>"), and an escaped '(' is not a
                // group (`/\k<a\(b)/` is a SyntaxError, matching Node).
                // '$' is not a stop: it is a valid name character.
                bool escaped = false;
                while (!IsEOF())
                {
                    if (escaped)
                    {
                        NextChar();
                        escaped = false;
                        continue;
                    }
                    switch (ECLookahead())
                    {
                    case '\\':
                        ECConsume();
                        escaped = true;
                        continue;
                    case '>':
                        ECConsume();
                        afterGt = next;
                        break;
                    case '(':
                    case ')':
                    case '|':
                    case '[':
                    case ']':
                    case '*':
                    case '+':
                    case '?':
                    case '^':
                    case '.':
                    case '/': // unescaped '/' terminates a regex literal body
                        break;
                    default:
                        NextChar();
                        continue;
                    }
                    break;
                }
                if (numNamedGroupRefSpans == namedGroupRefSpansCap)
                {
                    int newCap = namedGroupRefSpansCap == 0 ? 4 : namedGroupRefSpansCap * 2;
                    NamedGroupRefSpan* newList = AnewArray(ctAllocator, NamedGroupRefSpan, newCap);
                    for (int i = 0; i < numNamedGroupRefSpans; i++)
                        newList[i] = namedGroupRefSpans[i];
                    namedGroupRefSpans = newList;
                    namedGroupRefSpansCap = newCap;
                }
                NamedGroupRefSpan& span = namedGroupRefSpans[numNamedGroupRefSpans++];
                span.nameStart = nameStart;
                span.afterGt = afterGt;
            }
            else
            {
                sawBareK = true;
                ECConsume(); // k (identity escape for now)
            }
            return false;
        }
        else
        {
            const EncodedChar *current = next;
            // An escaped '/' is ok
            Char c = NextChar();
            switch (c)
            {
            case 'b':
            case 'B':
                return true;
            // case 'c': handled as special case above
            case 'x':
                if (ECCanConsume(2) &&
                    standardEncodedChars->IsHex(ECLookahead(0)) &&
                    standardEncodedChars->IsHex(ECLookahead(1)))
                    ECConsume(2);
                else if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                {
                    // \xNN is the only hex escape form under u (deferred;
                    // non-u keeps the identity-escape Annex B reading).
                    DeferredFailIfUnicode(JSERR_RegExpInvalidEscape);
                }
                break;
            case 'u':
                {
                    bool surrogateEncountered = false;
                    int lengthOfSurrogate = TryParseExtendedUnicodeEscape(c, surrogateEncountered, true);
                if (lengthOfSurrogate > 0)
                {
                    if (surrogateEncountered)
                    {
                        // If we don't have an allocator, we don't create nodes
                        // Asserts in place as extra checks for when we do have an allocator
                        Assert(this->ctAllocator == nullptr || this->currentSurrogatePairNode != nullptr);
                        Assert(this->ctAllocator == nullptr || current == this->currentSurrogatePairNode->location);
                        // The astral branch of TryParseExtendedUnicodeEscape
                        // rewound the cursor to the '\' to hand its first
                        // half to this scan; consuming the escape body only
                        // (lengthOfSurrogate, counted from '{') used to land
                        // two units short, leaving the escape tail to parse
                        // as literal terms. Consume the whole escape so the
                        // scan position matches the source text (the '}' and
                        // digit strict rules would otherwise see the tail).
                        ECConsume(lengthOfSurrogate + 2);
                    }
                    //Don't fall through
                    break;
                }
                else if (ECCanConsume(4) &&
                    standardEncodedChars->IsHex(ECLookahead(0)) &&
                    standardEncodedChars->IsHex(ECLookahead(1)) &&
                    standardEncodedChars->IsHex(ECLookahead(2)) &&
                    standardEncodedChars->IsHex(ECLookahead(3)))
                {
                    if (this->scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                    {
                        codepoint_t value = (standardEncodedChars->DigitValue(ECLookahead(0)) << 12) |
                                            (standardEncodedChars->DigitValue(ECLookahead(1)) << 8) |
                                            (standardEncodedChars->DigitValue(ECLookahead(2)) << 4) |
                                            (standardEncodedChars->DigitValue(ECLookahead(3)));
                        TrackIfSurrogatePair(value, (next - 1), 5);
                    }
                    ECConsume(4);
                }
                else if (this->scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                {
                    // Under u, \u must be uHex4 or u{CodePoint} (deferred;
                    // non-u keeps the identity-escape Annex B reading).
                    DeferredFailIfUnicode(JSERR_RegExpInvalidEscape);
                }
                break;
                }
            default:
                if (this->scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                {
                    // Under u an identity escape is limited to
                    // SyntaxCharacter plus '/' and the ZWNJ/ZWJ (ES2015
                    // IdentityEscape; the pseudo-escapes below reach their
                    // own pass-1 cases). Record the rejection here in pass
                    // 0 so deferred (no-AST) parses fail with a clean
                    // SyntaxError instead of an internal error on the
                    // later reparse; non-u keeps any-character Annex B
                    // identity escapes. Inside a v class span the legacy
                    // scan's leftover escapes are v class grammar (e.g.
                    // the \q of [[a]--\q{b}]): legal under v, rejected
                    // under plain u, Annex B identity without u.
                    bool inVClassSpan = this->vClassSpanStart != nullptr && next >= this->vClassSpanStart && next < this->vClassSpanEnd;
                    switch (c)
                    {
                    case '^':
                    case '$':
                    case '\\':
                    case '.':
                    case '*':
                    case '+':
                    case '?':
                    case '(':
                    case ')':
                    case '[':
                    case ']':
                    case '{':
                    case '}':
                    case '|':
                    case '/':
                    case 'f':
                    case 'n':
                    case 'r':
                    case 't':
                    case 'v':
                    case 'd':
                    case 'D':
                    case 's':
                    case 'S':
                    case 'w':
                    case 'W':
                        break;
                    default:
                        if (c == (Char)0x200C || c == (Char)0x200D) // ZWNJ / ZWJ
                        {
                            break;
                        }
                        if (inVClassSpan)
                        {
                            DeferredFailIfUnicodeNoSets(JSERR_RegExpInvalidEscape);
                        }
                        else
                        {
                            DeferredFailIfUnicode(JSERR_RegExpInvalidEscape);
                        }
                        break;
                    }
                }
                break;
            }
            // embedded 0 is ok

            return false;
        }
    }
#pragma warning(pop)

    template <typename P, const bool IsLiteral>
    bool Parser<P, IsLiteral>::AtomEscapePass1(Node*& node, MatchCharNode* deferredCharNode, bool& previousSurrogatePart)
    {
        Assert(!IsEOF()); // checked for terminating 0 in pass 0
        if (standardEncodedChars->IsDigit(ECLookahead()))
        {
            // As per Annex B, allow octal escapes as well as group references, disambiguate based on known
            // number of groups.
            if (ECLookahead() == '0')
            {
                // fall through for octal
            }
            else
            {
                // Could be a group reference, but only if between 1 and 5 digits which resolve to a valid group number
                int n = 0;
                CharCount digits = 0;
                do
                {
                    n = n * 10 + (int)standardEncodedChars->DigitValue(ECLookahead(digits));
                    digits++;
                }
                while (digits < 5 && ECCanConsume(digits + 1) && standardEncodedChars->IsDigit(ECLookahead(digits)));
                if (n >= numGroups ||
                    (ECCanConsume(digits + 1) && standardEncodedChars->IsDigit(ECLookahead(digits))))
                {
                    if (standardEncodedChars->IsOctal(ECLookahead()))
                    {
                        // fall through for octal
                    }
                    else
                    {
                        // \8 and \9 are identity escapes
                        deferredCharNode->cs[0] = Chars<EncodedChar>::CTW(ECLookahead());
                        ECConsume();
                        node = deferredCharNode;
                        return false; // not an assertion
                    }
                }
                else
                {
                    ECConsume(digits);
                    node = Anew(ctAllocator, MatchGroupNode, n, caseInsensitiveFlagPresent);
                    return false; // not an assertion
                }
            }

            // Must be between 1 and 3 octal digits
            Assert(standardEncodedChars->IsOctal(ECLookahead())); // terminating 0 is not an octal
            uint n = 0;
            CharCount digits = 0;
            do
            {
                uint m = n * 8  + standardEncodedChars->DigitValue(ECLookahead());
                if (m > Chars<uint8>::MaxUChar) // Regex octal codes only support single byte (ASCII) characters.
                    break;
                n = m;
                ECConsume();
                digits++;
            }
            while (digits < 3 && standardEncodedChars->IsOctal(ECLookahead())); // terminating 0 is not an octal

            deferredCharNode->cs[0] = UTC((UChar)n);
            node = deferredCharNode;
            return false; // not an assertion
        }
        else if (ECLookahead() == 'c')
        {
            Char c;
            if (standardEncodedChars->IsLetter(ECLookahead(1))) // terminating 0 is not a letter
            {
                c = UTC(Chars<EncodedChar>::CTU(ECLookahead(1)) % 32);
                ECConsume(2);
            }
            else
            {
                // SPEC DEVIATION: For non-letters or EOF, take the leading '\' to be itself, and
                //                 don't consume the 'c' or letter.
                c = '\\';
            }
            deferredCharNode->cs[0] = c;
            node = deferredCharNode;
            return false; // not an assertion
        }
        else if (ECLookahead() == 'k')
        {
            // Named backreference \k<Name>, or Annex B identity escape 'k'
            // when the pattern has no named captures and no u flag
            // (ResolveNamedGroupRefs already validated the reference form).
            if (this->numNamedGroups > 0 || this->unicodeFlagPresent)
            {
                ECConsume(); // k
                if (IsEOF() || ECLookahead() != '<')
                    Fail(JSERR_RegExpSyntax);
                ECConsume(); // <
                CharCount nameOffset = this->namedGroupBufferNext;
                ScanGroupName();
                const Char* name = this->namedGroupBuffer + nameOffset;
                const CharCount nameLen = this->namedGroupBufferNext - nameOffset;
                // ES2025 duplicate named groups: the name may be bound by
                // several groups (always in different alternatives of a
                // disjunction). The reference compiles to a
                // MatchNamedGroupNode that targets whichever of them
                // participates; a single-group name keeps the ordinary
                // numbered-backreference node. Definition order is
                // textual order, so the collected ids are in numeric
                // order too.
                int numIds = 0;
                for (int i = 0; i < this->numNamedGroups; i++)
                {
                    if (NamedGroupIsAt(this->namedGroups[i], name, nameLen))
                        numIds++;
                }
                if (numIds == 0)
                    Fail(JSERR_RegExpSyntax);
                if (numIds == 1)
                {
                    node = Anew(ctAllocator, MatchGroupNode, FindNamedGroup(name, nameLen), caseInsensitiveFlagPresent);
                }
                else
                {
                    int* groupIds = AnewArray(ctAllocator, int, numIds);
                    int nextId = 0;
                    for (int i = 0; i < this->numNamedGroups; i++)
                    {
                        if (NamedGroupIsAt(this->namedGroups[i], name, nameLen))
                            groupIds[nextId++] = this->namedGroups[i].groupId;
                    }
                    node = Anew(ctAllocator, MatchNamedGroupNode, numIds, groupIds, caseInsensitiveFlagPresent);
                }
            }
            else
            {
                // Identity escape; the following '<...>' (if any) parses as
                // literal atoms just like it did before this pass.
                deferredCharNode->cs[0] = 'k';
                ECConsume(); // k
                node = deferredCharNode;
            }
            return false; // not an assertion
        }
        else if ((ECLookahead() == 'p' || ECLookahead() == 'P') && this->unicodeFlagPresent)
        {
            // \p{...}/\P{...} property escape (u mode only; without the flag
            // 'p'/'P' falls through to the identity-escape switch below and
            // the '{...}' part re-parses as literal atoms).
            node = ParsePropertyEscapePass1();
            return false; // not an assertion
        }
        else
        {
            Char c = NextChar();
            switch (c)
            {
            case 'b':
                node = Anew(ctAllocator, WordBoundaryNode, false, lookbehindDepth > 0, unicodeFlagPresent && caseInsensitiveFlagPresent);
                return true; // Is an assertion
            case 'B':
                node = Anew(ctAllocator, WordBoundaryNode, true, lookbehindDepth > 0, unicodeFlagPresent && caseInsensitiveFlagPresent);
                return true; // Is an assertion
            case 'f':
                c = '\f';
                break; // fall-through for identity escape
            case 'n':
                c = '\n';
                break; // fall-through for identity escape
            case 'r':
                c = '\r';
                break; // fall-through for identity escape
            case 't':
                c = '\t';
                break; // fall-through for identity escape
            case 'v':
                c = '\v';
                break; // fall-through for identity escape
            case 'd':
                {
                    MatchSetNode *setNode = Anew(ctAllocator, MatchSetNode, false, false, caseInsensitiveFlagPresent);
                    standardChars->SetDigits(ctAllocator, setNode->set);
                    node = setNode;
                    return false; // not an assertion
                }
            case 'D':
                {
                    node = GetNodeWithValidCharacterSet('D');
                    return false; // not an assertion
                }
            case 's':
                {
                    MatchSetNode *setNode = Anew(ctAllocator, MatchSetNode, false, false, caseInsensitiveFlagPresent);
                    standardChars->SetWhitespace(ctAllocator, setNode->set);
                    node = setNode;
                    return false; // not an assertion
                }
            case 'S':
                {
                    node = GetNodeWithValidCharacterSet('S');
                    return false; // not an assertion
                }
            case 'w':
                {
                    MatchSetNode *setNode = Anew(ctAllocator, MatchSetNode, false, false, caseInsensitiveFlagPresent);
                    if (this->unicodeFlagPresent && this->caseInsensitiveFlagPresent)
                    {
                        standardChars->SetWordIUChars(ctAllocator, setNode->set);
                    }
                    else
                    {
                        standardChars->SetWordChars(ctAllocator, setNode->set);
                    }
                    node = setNode;
                    return false; // not an assertion
                }
            case 'W':
                {
                    node = GetNodeWithValidCharacterSet('W');
                    return false; // not an assertion
                }
            // case 'c': handled as special case above
            case 'x':
                if (ECCanConsume(2) &&
                    standardEncodedChars->IsHex(ECLookahead(0)) &&
                    standardEncodedChars->IsHex(ECLookahead(1)))
                {
                    c = UTC((standardEncodedChars->DigitValue(ECLookahead(0)) << 4) |
                        (standardEncodedChars->DigitValue(ECLookahead(1))));
                    ECConsume(2);
                    // fall-through for identity escape
                }
                // Take to be identity escape if ill-formed as per Annex B
                break;
            case 'u':
                if (unicodeFlagPresent && TryParseExtendedUnicodeEscape(c, previousSurrogatePart) > 0)
                    break;
                else if (ECCanConsume(4) &&
                    standardEncodedChars->IsHex(ECLookahead(0)) &&
                    standardEncodedChars->IsHex(ECLookahead(1)) &&
                    standardEncodedChars->IsHex(ECLookahead(2)) &&
                    standardEncodedChars->IsHex(ECLookahead(3)))
                {
                    c = UTC((standardEncodedChars->DigitValue(ECLookahead(0)) << 12) |
                            (standardEncodedChars->DigitValue(ECLookahead(1)) << 8) |
                            (standardEncodedChars->DigitValue(ECLookahead(2)) << 4) |
                            (standardEncodedChars->DigitValue(ECLookahead(3))));
                    ECConsume(4);
                    // fall-through for identity escape
                }
                // Take to be identity escape if ill-formed as per Annex B
                break;
            case '^':
            case '$':
            case '\\':
            case '.':
            case '*':
            case '+':
            case '?':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '|':
            case '/':
                break; // fall-through for identity escape
            default:
                if (this->unicodeFlagPresent)
                {
                    // As per #sec-forbidden-extensions, if unicode flag is present, we must disallow any other escape.
                    this->Fail(JSERR_RegExpInvalidEscape); // throw SyntaxError
                }
                // As per Annex B, allow anything other than newlines and above. Embedded 0 is ok
                break;
            }

            // Must be an identity escape
            deferredCharNode->cs[0] = c;
            node = deferredCharNode;
            return false; // not an assertion
        }
    }

    template <typename P, const bool IsLiteral>
    bool Parser<P, IsLiteral>::SurrogatePairPass1(Node*& node, MatchCharNode* deferredCharNode, bool& previousSurrogatePart)
    {
        if (!this->scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled() || !unicodeFlagPresent)
        {
            return false;
        }

        if (this->currentSurrogatePairNode != nullptr && this->currentSurrogatePairNode->location == this->next)
        {
            AssertMsg(!this->currentSurrogatePairNode->IsInsideRange(), "Should not be calling this pass if we are currently inside a range.");
            char16 lower, upper;

            uint tableIndex = 0, actualHigh = 0;
            codepoint_t equivClass[CaseInsensitive::EquivClassSize];

            if (caseInsensitiveFlagPresent && CaseInsensitive::RangeToEquivClass(tableIndex, this->currentSurrogatePairNode->value, this->currentSurrogatePairNode->value, actualHigh, equivClass))
            {
                Node *equivNode[CaseInsensitive::EquivClassSize];
                int indexForNextNode = 0;
                for (int i = 0; i < CaseInsensitive::EquivClassSize; i++)
                {
                    bool alreadyAdded = false;

                    for (int j = 0; j < i; j++)
                    {
                        if (equivClass[i] == equivClass[j])
                        {
                            alreadyAdded = true;
                            break;
                        }
                    }

                    if (!alreadyAdded)
                    {
                        if (Js::NumberUtilities::IsInSupplementaryPlane(equivClass[i]))
                        {
                            Js::NumberUtilities::CodePointAsSurrogatePair(equivClass[i], &lower, &upper);
                            equivNode[indexForNextNode] = CreateSurrogatePairAtom(lower, upper);
                        }
                        else
                        {
                            equivNode[indexForNextNode] = Anew(ctAllocator, MatchCharNode, (Char)equivClass[i], caseInsensitiveFlagPresent);
                        }
                        indexForNextNode ++;
                    }
                }
                Assert(indexForNextNode > 0);
                if (indexForNextNode == 1)
                {
                    node = equivNode[0];
                }
                else
                {
                    AltNode *altNode = Anew(ctAllocator, AltNode, equivNode[0], nullptr);
                    AltNode *altNodeTail = altNode;
                    for (int i = 1; i < indexForNextNode; i++)
                    {
                        altNodeTail->tail = Anew(ctAllocator, AltNode, equivNode[i], nullptr);
                        altNodeTail = altNodeTail->tail;
                    }
                    node = altNode;
                }
            }
            else
            {
                Js::NumberUtilities::CodePointAsSurrogatePair(this->currentSurrogatePairNode->value, &lower, &upper);
                node = CreateSurrogatePairAtom(lower, upper);
            }

            previousSurrogatePart = false;

            Assert(ECCanConsume(this->currentSurrogatePairNode->length));
            ECConsumeMultiUnit(this->currentSurrogatePairNode->length);
            this->RestoreMultiUnits(this->currentSurrogatePairNode->multiUnits);
            this->currentSurrogatePairNode = this->currentSurrogatePairNode->next;

            return true;
        }

        return false;
    }

    //
    // Classes
    //

    template <typename P, const bool IsLiteral>
    bool Parser<P, IsLiteral>::AtSecondSingletonClassAtom()
    {
        Assert(ECLookahead() == '-');
        if (ECLookahead(1) == '\\')
        {
            switch (ECLookahead(2))
            {
            case 'd':
            case 'D':
            case 's':
            case 'S':
            case 'w':
            case 'W':
                // These all denote non-singleton sets
                return false;
            default:
                // fall-through for singleton
                break;
            }
        }
        return true;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::CharacterClassPass0()
    {
        // Could be terminating 0
        if (ECLookahead() == '^')
            ECConsume();

        EncodedChar nextChar = ECLookahead();
        const EncodedChar* current;
        codepoint_t lastCodepoint = INVALID_CODEPOINT;
        codepoint_t pendingRangeStart = INVALID_CODEPOINT;
        codepoint_t pendingRangeEnd = INVALID_CODEPOINT;
        bool previousSurrogatePart = false;

        while(nextChar != ']')
        {
            current = next;

            if (nextChar == '\0' && IsEOF())
            {
                // Report as unclosed '['
                Fail(JSERR_RegExpNoBracket);
                return;
            } // Otherwise embedded '\0' is ok
            else if (nextChar == '\\')
            {
                // Consume, as classescapepass0 expects for it to be consumed
                Char outChar = NextChar();
                // If previousSurrogatePart = true upon leaving this method, then we are going to pass through here twice
                // This is because \u{} escape sequence was encountered that is actually 2 characters, the second time we will pass consuming entire character
                if (ClassEscapePass0(outChar, previousSurrogatePart))
                {
                    lastCodepoint = outChar;
                }
                else
                {
                    // Last codepoint isn't a singleton, so no codepoint tracking for the sake of ranges is needed.
                    lastCodepoint = INVALID_CODEPOINT;
                    // Unless we have a possible range end, cancel our range tracking.
                    if (pendingRangeEnd == INVALID_CODEPOINT)
                    {
                        pendingRangeStart = INVALID_CODEPOINT;
                    }
                }
            }
            else if (nextChar == '-')
            {
                if (pendingRangeStart != INVALID_CODEPOINT || lastCodepoint == INVALID_CODEPOINT)
                {
                    // '-' is the upper part of the range, with pendingRangeStart codepoint is the lower. Set  lastCodePoint to '-' to check at the end of the while statement
                    // OR
                    // '-' is just a char, consume it and set as last char
                    lastCodepoint = NextChar();
                }
                else if (ECLookahead(1) == '-' &&
                         scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled() &&
                         (lastCodepoint > (codepoint_t)'-' ||
                          this->next == this->positionAfterLastSurrogate))
                {
                    // v flag (ES2024): '<singleton>--' where the legacy range
                    // <singleton>..'-' would be invalid, or a completed
                    // surrogate pair followed by '--', starts a set
                    // difference operator instead. When the range would be
                    // VALID (e.g. '[+--]' is the range +..-) the legacy range
                    // stands: u keeps it and v's pass 1 re-parses the text as
                    // an (invalid or operator-shaped) v class. Without the
                    // surrogate case, the pair position turns the first dash
                    // into range start U+1F600.. and the surrogate-range
                    // machinery records a deferred BadRange that fires under
                    // v (the astral-operand review bug). The u flag is
                    // unknown in pass 0, so consume BOTH dashes as literal
                    // characters and defer the range error for every non-v
                    // mode (they reject these shapes today; only v parses
                    // on). Consuming both dashes also prevents the second
                    // one from opening a new (bogus) range against the
                    // following character, which would hard-fail patterns
                    // like '[a--\t]'.
                    DeferredFailIfNotUnicode(JSERR_RegExpBadRange);
                    DeferredFailIfUnicodeNoSets(JSERR_RegExpBadRange);
                    NextChar(); // first dash
                    NextChar(); // second dash
                    lastCodepoint = INVALID_CODEPOINT;
                    this->tempLocationOfSurrogatePair = nullptr;
                }
                else
                {
                    pendingRangeStart = this->next == this->positionAfterLastSurrogate
                        ? this->valueOfLastSurrogate
                        : lastCodepoint;
                    lastCodepoint = INVALID_CODEPOINT;
                    NextChar();
                }

                // If we have a pattern of the form [\ud800-\udfff], we need this to be interpreted as a range.
                // In order to achieve this, the two variables that we use to track surrogate pairs, namely
                // tempLocationOfSurrogatePair and previousSurrogatePart, need to be in a certain state.
                //
                // We need to reset tempLocationOfSurrogatePair as it points to the first Unicode escape (\ud800)
                // when we're here. We need to clear it in order not to have a surrogate pair when we process the
                // second escape (\udfff).
                //
                // previousSurrogatePart is used when we have a code point in the \u{...} extended format and the
                // character is in a supplementary plane. However, there is no need to change its value here. When
                // such an escape sequence is encountered, the first call to ClassEscapePass0() sets the variable
                // to true, but it rewinds the input back to the beginning of the escape sequence. The next
                // iteration of the loop here will again call ClassEscape0() with the same character and the
                // variable will this time be set to false. Therefore, the variable will always be false here.
                tempLocationOfSurrogatePair = nullptr;
                Assert(!previousSurrogatePart);
            }
            else
            {
                lastCodepoint = NextChar();

                if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                {
                    TrackIfSurrogatePair(lastCodepoint, current, (uint32)(next - current));
                }
            }

            if (!scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
            {
                // This is much easier to handle.
                if (pendingRangeStart != INVALID_CODEPOINT && lastCodepoint != INVALID_CODEPOINT)
                {
                    Assert(pendingRangeStart < 0x10000);
                    Assert(pendingRangeEnd == INVALID_CODEPOINT);
                    if (pendingRangeStart > lastCodepoint)
                    {
                        Fail(JSERR_RegExpBadRange);
                    }
                    pendingRangeStart = lastCodepoint = INVALID_CODEPOINT;
                }
            }
            // We have a candidate for range end, and we have a range start.
            else if (pendingRangeStart != INVALID_CODEPOINT && (lastCodepoint != INVALID_CODEPOINT || pendingRangeEnd != INVALID_CODEPOINT))
            {
                // The following will be true at the end of each surrogate pair parse.
                // Note, the escape sequence \u{} is a two time parse, so this will be true on the second time around.
                if (this->next == this->positionAfterLastSurrogate)
                {
                    lastCodepoint = this->valueOfLastSurrogate;
                    Assert(!previousSurrogatePart);
                    pendingRangeEnd = lastCodepoint;

                    lastCodepoint = INVALID_CODEPOINT;
                }
                // If the next character is the end of range ']', then we can't have a surrogate pair.
                // The current character is the range end, if we don't already have a candidate.
                else if (ECLookahead() == ']' && pendingRangeEnd == INVALID_CODEPOINT)
                {
                    pendingRangeEnd = lastCodepoint;
                }

                //If we get here, and pendingRangeEnd is set. Then one of the above has caused it to be set, or the previous iteration of the loop.
                if (pendingRangeEnd != INVALID_CODEPOINT)
                {
                    char16 leftSingleChar, rightSingleChar, ignore;

                    if (pendingRangeStart >= 0x10000)
                    {
                        Js::NumberUtilities::CodePointAsSurrogatePair(pendingRangeStart, &ignore, &leftSingleChar);
                    }
                    else
                    {
                        leftSingleChar = (char16)pendingRangeStart;
                    }

                    if (pendingRangeEnd >= 0x10000)
                    {
                        Js::NumberUtilities::CodePointAsSurrogatePair(pendingRangeEnd, &rightSingleChar, &ignore);
                    }
                    else
                    {
                        rightSingleChar = (char16)pendingRangeEnd;
                    }

                    // Here it is a bit tricky, we don't know if we have a unicode option specified.
                    // If it is, then \ud800\udc00 - \ud800\udc01 is valid, otherwise invalid.
                    if (pendingRangeStart < 0x10000 && pendingRangeEnd < 0x10000 && pendingRangeStart > pendingRangeEnd)
                    {
                        if (pendingRangeStart == (codepoint_t)'-' &&
                            pendingRangeEnd < (codepoint_t)'-' &&
                            scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                        {
                            // v flag (ES2024): a range whose start is the
                            // literal '-' can only come from a '--' operator
                            // shape ('<set>--<operand>' or '\--<operand>'):
                            // a legacy range starting at '-' with a smaller
                            // end is invalid in every mode, so defer the
                            // error for non-v modes (they keep rejecting it)
                            // and let v's pass 1 re-parse the operator.
                            DeferredFailIfNotUnicode(JSERR_RegExpBadRange);
                            DeferredFailIfUnicodeNoSets(JSERR_RegExpBadRange);
                            pendingRangeStart = pendingRangeEnd = INVALID_CODEPOINT;
                        }
                        else
                        {
                            Fail(JSERR_RegExpBadRange);
                        }
                    }
                    else
                    {
                        if(leftSingleChar > rightSingleChar)
                        {
                            DeferredFailIfNotUnicode(JSERR_RegExpBadRange);
                        }

                        if (pendingRangeStart > pendingRangeEnd)
                        {
                            DeferredFailIfUnicode(JSERR_RegExpBadRange);
                        }
                    }

                    pendingRangeStart = pendingRangeEnd = INVALID_CODEPOINT;
                }
                // The current char < 0x10000 is a candidate for the range end, but we need to iterate one more time.
                else
                {
                    pendingRangeEnd = lastCodepoint;
                }
            }
            nextChar = ECLookahead();
        }

        // We should never have a pendingRangeEnd set when we exit the loop
        Assert(pendingRangeEnd == INVALID_CODEPOINT);
    }


    template <typename P, const bool IsLiteral>
    template <bool containsSurrogates>
    Node* Parser<P, IsLiteral>::CharacterClassPass1()
    {
        Assert(containsSurrogates ? unicodeFlagPresent : true);

        CharSet<codepoint_t> codePointSet;

        MatchSetNode deferredSetNode(false, false, caseInsensitiveFlagPresent);
        MatchCharNode deferredCharNode(0, caseInsensitiveFlagPresent);

        // [^\D]/[^\S]/[^\W] (sole complement shorthand under negation)
        // equal \d/\s/\w over all code points; see the escape branch.
        bool sawAnyClassItem = false;
        EncodedChar negatedSoleShorthand = 0;

        bool isNegation = false;

        if (ECLookahead() == '^')
        {
            isNegation = true;
            ECConsume();
        }

        // We aren't expecting any terminating null characters, only embedded ones that should treated as valid characters.
        // CharacterClassPass0 should have taken care of terminating null.
        codepoint_t pendingCodePoint = INVALID_CODEPOINT;
        codepoint_t pendingRangeStart = INVALID_CODEPOINT;
        EncodedChar nextChar = ECLookahead();
        bool previousWasASurrogate = false;
        bool currIsACharSet = false;
        bool prevWasACharSetAndPartOfRange = false;
        bool prevprevWasACharSetAndPartOfRange = false;

        while(nextChar != ']')
        {
            codepoint_t codePointToSet = INVALID_CODEPOINT;

            // Consume ahead of time if we have two backslashes, both cases below (previously Tracked surrogate pair, and ClassEscapePass1) assume it is.
            if (nextChar == '\\')
            {
                ECConsume();
            }

            // These if-blocks are the logical ClassAtomPass1, they weren't grouped into a method to simplify dealing with multiple out parameters.
            if (containsSurrogates && this->currentSurrogatePairNode != nullptr && this->currentSurrogatePairNode->location == this->next)
            {
                codePointToSet = pendingCodePoint;

                pendingCodePoint = this->currentSurrogatePairNode->value;
                Assert(ECCanConsume(this->currentSurrogatePairNode->length));
                ECConsumeMultiUnit(this->currentSurrogatePairNode->length);
                this->RestoreMultiUnits(this->currentSurrogatePairNode->multiUnits);
                this->currentSurrogatePairNode = this->currentSurrogatePairNode->next;
            }
            else if (nextChar == '\\')
            {
                if (unicodeFlagPresent && (ECLookahead() == 'p' || ECLookahead() == 'P') && ECLookahead(1) == '{')
                {
                    // \p{...}/\P{...} inside the class: union its code point
                    // set. Like \d & co it is a set-valued atom, so it cannot
                    // be a range endpoint. (Without the brace, pass 0's
                    // deferred invalid-escape error already rejected the
                    // u-mode pattern, and non-u re-parses as identity.)
                    if (pendingRangeStart != INVALID_CODEPOINT)
                    {
                        Fail(JSERR_UnicodeRegExpRangeContainsCharClass);
                    }
                    UnionPropertyEscapePass1(codePointSet);
                    codePointToSet = pendingCodePoint;
                    pendingCodePoint = INVALID_CODEPOINT;
                    sawAnyClassItem = true;
                    currIsACharSet = true;
                }
                else
                {
                    Node* returnedNode = ClassEscapePass1(&deferredCharNode, &deferredSetNode, previousWasASurrogate);
                    codePointToSet = pendingCodePoint;

                    if (returnedNode->tag == Node::MatchSet)
                    {
                        if (pendingRangeStart != INVALID_CODEPOINT)
                        {
                            if (unicodeFlagPresent)
                            {
                                //A range containing a character class and the unicode flag is present, thus we end up having to throw a "Syntax" error here
                                //This breaks the notion of Pass0 check for valid syntax, because during that time, the unicode flag is unknown.
                                Fail(JSERR_UnicodeRegExpRangeContainsCharClass); //From #sec-patterns-static-semantics-early-errors-annexb
                            }

                            codePointSet.Set(ctAllocator, '-');
                        }

                        pendingCodePoint = INVALID_CODEPOINT;
                        pendingRangeStart = INVALID_CODEPOINT;
                        codePointSet.UnionInPlace(ctAllocator, deferredSetNode.set);
                        // Track a sole complement shorthand (\D, \S, \W):
                        // a negated class containing exactly one of them
                        // ([^\D] etc.) must equal the positive shorthand
                        // over ALL code points, but the char16 set only
                        // carries the BMP part and the negated unit-set
                        // substrate would admit astral input (the review's
                        // astral-complement bug). The flip below rebuilds
                        // the positive set.
                        if (unicodeFlagPresent &&
                            (ECLookback(1) == 'D' || ECLookback(1) == 'S' || ECLookback(1) == 'W'))
                        {
                            if (sawAnyClassItem || negatedSoleShorthand != 0)
                                negatedSoleShorthand = 0; // not sole (sibling before or after)
                            else
                                negatedSoleShorthand = ECLookback(1);
                            // The complement shorthands are defined over
                            // ALL code points, but the char16 set carries
                            // only the BMP part. Without the astral planes a
                            // negated class with siblings ([^\W_]) would
                            // complement the astral gap back in and match
                            // astral input; positive unions ([a\D]) gain
                            // the astral members they must have.
                            codePointSet.SetRange(ctAllocator, 0x10000, 0x10FFFF);
                        }
                        else
                        {
                            sawAnyClassItem = true;
                        }
                        currIsACharSet = true;
                    }
                    else
                    {
                        // Just a character
                        pendingCodePoint = deferredCharNode.cs[0];
                    }
                }
            }
            else if (nextChar == '-')
            {
                if (pendingRangeStart != INVALID_CODEPOINT || pendingCodePoint == INVALID_CODEPOINT || ECLookahead(1) == ']')
                {
                    // - is just a char, or end of a range.
                    codePointToSet = pendingCodePoint;
                    pendingCodePoint = '-';
                    ECConsume();
                }
                else
                {
                    pendingRangeStart = pendingCodePoint;
                    ECConsume();
                }
            }
            else
            {
                // Just a character, consume it
                codePointToSet = pendingCodePoint;
                pendingCodePoint = NextChar();
            }

            if (codePointToSet != INVALID_CODEPOINT || prevprevWasACharSetAndPartOfRange)
            {
                if (prevprevWasACharSetAndPartOfRange)
                {
                    //A range containing a character class and the unicode flag is present, thus we end up having to throw a "Syntax" error here
                    //This breaks the notion of Pass0 check for valid syntax, because during that time, the unicode flag is unknown.
                    if (unicodeFlagPresent)
                    {
                        Fail(JSERR_UnicodeRegExpRangeContainsCharClass);
                    }

                    if (pendingCodePoint != INVALID_CODEPOINT)
                    {
                        codePointSet.Set(ctAllocator, pendingCodePoint);
                    }

                    codePointSet.Set(ctAllocator, '-'); //Add '-' to set because a range was detected but turned out to be a union of character set with '-' and another atom.
                    pendingRangeStart = pendingCodePoint = INVALID_CODEPOINT;
                }
                else if (pendingRangeStart != INVALID_CODEPOINT)
                {
                    if (pendingRangeStart > pendingCodePoint)
                    {
                        //We have no unicodeFlag, but current range contains surrogates, thus we may end up having to throw a "Syntax" error here
                        //This breaks the notion of Pass0 check for valid syntax, because we don't know if we have a unicode option
                        Assert(!unicodeFlagPresent);
                        Fail(JSERR_RegExpBadRange);
                    }
                    
                    codePointSet.SetRange(ctAllocator, pendingRangeStart, pendingCodePoint);
                    pendingRangeStart = pendingCodePoint = INVALID_CODEPOINT;
                    sawAnyClassItem = true;
                }
                else
                {
                    codePointSet.Set(ctAllocator, codePointToSet);
                    sawAnyClassItem = true;
                }
            }

            nextChar = ECLookahead();
            prevprevWasACharSetAndPartOfRange = prevWasACharSetAndPartOfRange;
            prevWasACharSetAndPartOfRange = currIsACharSet && nextChar == '-';
            currIsACharSet = false;
        }

        if (pendingCodePoint != INVALID_CODEPOINT)
        {
            codePointSet.Set(ctAllocator, pendingCodePoint);
            sawAnyClassItem = true;
        }

        // At this point, we have a complete set of codepoints representing the range.
        // Before performing translation of any kind, we need to do some case filling.
        // At the point of this comment, there are no case mappings going cross-plane between simple
        // characters (< 0x10000) and supplementary characters (>= 0x10000)
        // However, it might still be the case, and this has to be handled.

        // On the other hand, we don't want to prevent optimizations that expect non-casefolded sets from happening.
        // At least for simple characters.

        // The simple case, is when the unicode flag isn't specified, we can go ahead and return the simple set.
        // Negations and case mappings will be handled later.
        if (!unicodeFlagPresent)
        {
            Assert(codePointSet.SimpleCharCount() == codePointSet.Count());
            MatchSetNode *simpleToReturn = Anew(ctAllocator, MatchSetNode, isNegation, true, caseInsensitiveFlagPresent);
            codePointSet.CloneSimpleCharsTo(ctAllocator, simpleToReturn->set);
            return simpleToReturn;
        }

        // Everything past here must be under the flag
        Assert(scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled());

        // [^\D] == \d over all code points: flip the sole complement
        // shorthand to its positive form so the negated unit-set substrate
        // never sees it (it would otherwise match astral input).
        if (isNegation && negatedSoleShorthand != 0 && !sawAnyClassItem)
        {
            codePointSet.Clear(ctAllocator);
            CharSet<Char> units;
            switch (negatedSoleShorthand)
            {
                case 'D': standardChars->SetDigits(ctAllocator, units); break;
                case 'S': standardChars->SetWhitespace(ctAllocator, units); break;
                case 'W':
                    if (caseInsensitiveFlagPresent)
                        standardChars->SetWordIUChars(ctAllocator, units);
                    else
                        standardChars->SetWordChars(ctAllocator, units);
                    break;
            }
            Char lo = 0, hi = 0;
            Char search = 0;
            while (units.GetNextRange(search, &lo, &hi))
            {
                codePointSet.SetRange(ctAllocator, lo, hi);
                if (hi == Chars<Char>::MaxUChar)
                    break;
                search = hi + 1;
            }
            units.FreeBody(ctAllocator);
            isNegation = false;
        }

        // If negation, complement over all code points; under i, close over
        // case equivalence classes first (so [^\p{Lu}]iu does not match 'A',
        // while the atom form \P{Lu} closes after complementing — see
        // ParsePropertyEscapePass1).
        CharSet<codepoint_t> negatedSet;
        CharSet<codepoint_t> *toUseForTranslation = &codePointSet;

        if (!this->caseInsensitiveFlagPresent)
        {
            if (isNegation)
            {
                // Complement all characters, and use it as the set toTranslate
                codePointSet.ToComplement(ctAllocator, negatedSet);
            }

            toUseForTranslation = isNegation ? &negatedSet : &codePointSet;

            if (isNegation)
            {
                // Clear this, as we will no longer need this.
                codePointSet.FreeBody(ctAllocator);
            }
        }
        else
        {
            CharSet<codepoint_t> caseEquivalent;
            codePointSet.ToEquivClass(ctAllocator, caseEquivalent);
            // Equiv set can't have a reduced count of chars
            Assert(caseEquivalent.Count() >= codePointSet.Count());

            // Here we have a regex that has both case insensitive and unicode options.
            // The range might also be negated. If it is negated, we can go ahead and negate
            // the entire set as well as fill in cases, as optimizations wouldn't kick in anyways.
            if (isNegation)
            {
                codePointSet.Clear(ctAllocator);
                caseEquivalent.ToComplement(ctAllocator, codePointSet);
                caseEquivalent.FreeBody(ctAllocator);
            }
            else
            {
                codePointSet.CloneFrom(ctAllocator, caseEquivalent);
            }

            Assert(toUseForTranslation == &codePointSet);
        }

        // TranslateCodePointSet clears the translated set itself; under
        // negation the class set was already freed by the complement prep
        // above (matching the original inline code exactly). A negated
        // u-mode class must not match the lead half of an excluded complete
        // surrogate pair, so it carries noHalfPairFallback.
        return TranslateCodePointSet(*toUseForTranslation, isNegation, /*noHalfPairFallback*/ isNegation);
    }
#pragma warning(push)
#pragma warning(disable:4702)   // unreachable code
    template <typename P, const bool IsLiteral>
    bool Parser<P, IsLiteral>::ClassEscapePass0(Char& singleton, bool& previousSurrogatePart)
    {
        // Could be terminating 0
        EncodedChar ec = ECLookahead();
        if (ec == 0 && IsEOF())
        {
            Fail(JSERR_RegExpSyntax);
            return false;
        }
        else if (standardEncodedChars->IsOctal(ec))
        {
            uint n = 0;
            CharCount digits = 0;
            do
            {
                uint m = n * 8  + standardEncodedChars->DigitValue(ECLookahead());
                if (m > Chars<uint8>::MaxUChar) //Regex octal codes only support single byte (ASCII) characters.
                    break;
                n = m;
                ECConsume();
                digits++;
            }
            while (digits < 3 && standardEncodedChars->IsOctal(ECLookahead())); // terminating 0 is not octal
            if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
            {
                // ClassEscape under u allows decimal 0 only when no decimal
                // digit follows; every legacy octal shape (and \0-started
                // runs, whose extra digit was already consumed above) is a
                // deferred u-mode rejection. Non-u keeps the octal reading.
                if (digits != 1 || n != 0 || standardEncodedChars->IsDigit(ECLookahead()))
                {
                    DeferredFailIfUnicode(JSERR_RegExpInvalidEscape);
                }
            }
            singleton = UTC((UChar)n);
            // Clear possible pair
            this->tempLocationOfSurrogatePair = nullptr;
            return true;
        }
        else
        {
            const EncodedChar* location = this->tempLocationOfSurrogatePair;
            // Clear it for now, otherwise to many branches to clear it on.
            this->tempLocationOfSurrogatePair = nullptr;
            // An escaped '/' is ok
            Char c = NextChar();
            switch (c)
            {
            case 'b':
                singleton = '\b';
                return true;
            case 'f':
                singleton = '\f';
                return true;
            case 'n':
                singleton = '\n';
                return true;
            case 'r':
                singleton = '\r';
                return true;
            case 't':
                singleton = '\t';
                return true;
            case 'v':
                singleton = '\v';
                return true;
            case 'd':
            case 'D':
            case 's':
            case 'S':
            case 'w':
            case 'W':
                return false;
            case 'c':
                if (!standardEncodedChars->IsLetter(ECLookahead())) //Letter set [A-Z, a-z]
                {
                    // Fail in unicode mode for non-letter escaped control characters according to 262 Annex-B RegExp grammar spec #prod-annexB-Term 
                    DeferredFailIfUnicode(JSERR_RegExpInvalidEscape);
                }

                if (standardEncodedChars->IsWord(ECLookahead())) // word set [A-Z,a-z,0-9,_], terminating 0 is not a word character
                {
                    singleton = UTC(Chars<EncodedChar>::CTU(ECLookahead()) % 32);
                    ECConsume();
                }
                else
                {
                    // If the lookahead is a non-alphanumeric and not an underscore ('_'), then treat '\' and 'c' separately.
                    //#sec-regular-expression-patterns-semantics 
                    ECRevert(1); //Put cursor back at 'c' and treat it as a non-escaped character.
                    singleton = '\\';
                }
                return true;
            case 'x':
                if (ECCanConsume(2) &&
                    standardEncodedChars->IsHex(ECLookahead(0)) &&
                    standardEncodedChars->IsHex(ECLookahead(1)))
                {
                    singleton = UTC((standardEncodedChars->DigitValue(ECLookahead(0)) << 4) |
                            (standardEncodedChars->DigitValue(ECLookahead(1))));
                    ECConsume(2);
                }
                else
                {
                    if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                    {
                        // \xNN is the only hex escape form under u
                        DeferredFailIfUnicode(JSERR_RegExpInvalidEscape);
                    }
                    singleton = c;
                }
                return true;
            case 'u':
                this->tempLocationOfSurrogatePair = location;
                if (this->TryParseExtendedUnicodeEscape(singleton, previousSurrogatePart, true) > 0)
                    return true;
                else if (ECCanConsume(4) &&
                    standardEncodedChars->IsHex(ECLookahead(0)) &&
                    standardEncodedChars->IsHex(ECLookahead(1)) &&
                    standardEncodedChars->IsHex(ECLookahead(2)) &&
                    standardEncodedChars->IsHex(ECLookahead(3)))
                {
                    singleton = UTC((standardEncodedChars->DigitValue(ECLookahead(0)) << 12) |
                            (standardEncodedChars->DigitValue(ECLookahead(1)) << 8) |
                            (standardEncodedChars->DigitValue(ECLookahead(2)) << 4) |
                            (standardEncodedChars->DigitValue(ECLookahead(3))));
                    if (this->scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                    {
                        // Current location
                        TrackIfSurrogatePair(singleton, (next - 1), 5);
                    }
                    // The above if statement, if true, will clear tempLocationOfSurrogatePair if needs to.
                    ECConsume(4);
                }
                else
                {
                    if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                    {
                        // Under u, \u must be uHex4 or u{CodePoint}
                        DeferredFailIfUnicode(JSERR_RegExpInvalidEscape);
                    }
                    singleton = c;
                }
                return true;
            case 'k':
                // There is no \k<Name> backreference inside classes. '\k' is
                // the Annex B identity escape 'k' only in non-u patterns with
                // no named captures (Annex B reserves 'k' for references once
                // named captures appear, and u mode has no identity escape for
                // it); ResolveNamedGroupRefs rejects the other combinations.
                sawClassK = true;
                singleton = c;
                return true;
            case 'p':
            case 'P':
                // \p{...}/\P{...} property escape (u mode); like \d & co a
                // property escape with a body is never a singleton class
                // atom. 'p'/'P' was already consumed by the switch's
                // NextChar, so the shared scanner continues from '{'.
                // Without the brace this stays the Annex B identity escape
                // 'p'/'P' (an invalid escape once the u flag is known).
                if (ECLookahead() == '{')
                {
                    ScanPropertyEscapeBodyPass0(/*isNegation*/ c == 'P');
                    return false;
                }
                DeferredFailIfUnicode(JSERR_RegExpInvalidEscape);
                singleton = c;
                return true;
            default:
                if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                {
                    // Under u a class identity escape is limited to
                    // SyntaxCharacter plus '-' and '/' (ES2015
                    // ClassEscape/IdentityEscape; Annex B accepts any
                    // character). Everything else ([\M], [\8], [\9], ...)
                    // is a deferred rejection. Deferred-if-unicode-WITHOUT-v:
                    // the v grammar has its own class-escape policy (its
                    // validator already rejects word characters and octals,
                    // while escaped reserved singles like [\&] stay legal).
                    switch (c)
                    {
                    case '^':
                    case '$':
                    case '\\':
                    case '.':
                    case '*':
                    case '+':
                    case '?':
                    case '(':
                    case ')':
                    case '[':
                    case ']':
                    case '{':
                    case '}':
                    case '|':
                    case '-':
                    case '/':
                        break;
                    default:
                        DeferredFailIfUnicodeNoSets(JSERR_RegExpInvalidEscape);
                        break;
                    }
                }
                // embedded 0 is ok
                singleton = c;
                return true;
            }
        }
    }
#pragma warning(pop)

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::ClassEscapePass1(MatchCharNode* deferredCharNode, MatchSetNode* deferredSetNode, bool& previousSurrogatePart)
    {
        // Checked for terminating 0 is pass 0
        Assert(!IsEOF());
        if (standardEncodedChars->IsOctal(ECLookahead()))
        {
            // As per Annex B, allow octal escapes instead of just \0 (and \8 and \9 are identity escapes).
            // Must be between 1 and 3 octal digits.
            uint n = 0;
            CharCount digits = 0;
            do
            {
                uint m = n * 8  + standardEncodedChars->DigitValue(ECLookahead());
                if (m > Chars<uint8>::MaxUChar) //Regex octal codes only support single byte (ASCII) characters.
                    break;
                n = m;
                ECConsume();
                digits++;
            }
            while (digits < 3 && standardEncodedChars->IsOctal(ECLookahead())); // terminating 0 is not octal
            deferredCharNode->cs[0] = UTC((UChar)n);
            return deferredCharNode;
        }
        else
        {
            Char c = NextChar();
            switch (c)
            {
            case 'b':
                c = '\b';
                break; // fall-through for identity escape
            case 'f':
                c = '\f';
                break; // fall-through for identity escape
            case 'n':
                c = '\n';
                break; // fall-through for identity escape
            case 'r':
                c = '\r';
                break; // fall-through for identity escape
            case 't':
                c = '\t';
                break; // fall-through for identity escape
            case 'v':
                c = '\v';
                break; // fall-through for identity escape
            case 'd':
                standardChars->SetDigits(ctAllocator, deferredSetNode->set);
                return deferredSetNode;
            case 'D':
                standardChars->SetNonDigits(ctAllocator, deferredSetNode->set);
                return deferredSetNode;
            case 's':
                standardChars->SetWhitespace(ctAllocator, deferredSetNode->set);
                return deferredSetNode;
            case 'S':
                standardChars->SetNonWhitespace(ctAllocator, deferredSetNode->set);
                return deferredSetNode;
            case 'w':
                if (unicodeFlagPresent && caseInsensitiveFlagPresent)
                {
                    standardChars->SetWordIUChars(ctAllocator, deferredSetNode->set);
                }
                else
                {
                    standardChars->SetWordChars(ctAllocator, deferredSetNode->set);
                }
                return deferredSetNode;
            case 'W':
                if (unicodeFlagPresent && caseInsensitiveFlagPresent)
                {
                    standardChars->SetNonWordIUChars(ctAllocator, deferredSetNode->set);
                }
                else
                {
                    standardChars->SetNonWordChars(ctAllocator, deferredSetNode->set);
                }
                return deferredSetNode;
            case 'c':
                if (standardEncodedChars->IsWord(ECLookahead())) // terminating 0 is not a word character
                {
                    c = UTC(Chars<EncodedChar>::CTU(ECLookahead()) % 32);
                    ECConsume();
                    // fall-through for identity escape
                }
                else
                {
                    // If the lookahead is a non-alphanumeric and not an underscore ('_'), then treat '\' and 'c' separately.
                    //#sec-regular-expression-patterns-semantics 
                    ECRevert(1); //Put cursor back at 'c' and treat it as a non-escaped character.
                    deferredCharNode->cs[0] = '\\';
                    return deferredCharNode;
                }
                break;
            case 'x':
                if (ECCanConsume(2) &&
                    standardEncodedChars->IsHex(ECLookahead(0)) &&
                    standardEncodedChars->IsHex(ECLookahead(1)))
                {
                    c = UTC((standardEncodedChars->DigitValue(ECLookahead(0)) << 4) |
                            (standardEncodedChars->DigitValue(ECLookahead(1))));
                    ECConsume(2);
                    // fall-through for identity escape
                }
                // Take to be identity escape if ill-formed as per Annex B
                break;
            case 'u':
                if (unicodeFlagPresent && TryParseExtendedUnicodeEscape(c, previousSurrogatePart) > 0)
                    break;
                else if (ECCanConsume(4) &&
                    standardEncodedChars->IsHex(ECLookahead(0)) &&
                    standardEncodedChars->IsHex(ECLookahead(1)) &&
                    standardEncodedChars->IsHex(ECLookahead(2)) &&
                    standardEncodedChars->IsHex(ECLookahead(3)))
                {
                    c = UTC((standardEncodedChars->DigitValue(ECLookahead(0)) << 12) |
                            (standardEncodedChars->DigitValue(ECLookahead(1)) << 8) |
                            (standardEncodedChars->DigitValue(ECLookahead(2)) << 4) |
                            (standardEncodedChars->DigitValue(ECLookahead(3))));
                    ECConsume(4);
                    // fall-through for identity escape
                }
                // Take to be identity escape if ill-formed as per Annex B.
                break;
            default:
                // As per Annex B, allow anything other than newlines and above. Embedded 0 is ok.
                break;
            }

            // Must be an identity escape
            deferredCharNode->cs[0] = c;
            return deferredCharNode;
        }
    }

    //
    // Options
    //

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::Options(RegexFlags& flags)
    {
        while (true)
        {
            // Could be terminating 0
            EncodedChar ec = ECLookahead();
            CharCount consume;
            Char c;

            if (ec == 0)
                // Embedded 0 not valid
                return;
            else if (IsLiteral &&
                ec == '\\' &&
                ECCanConsume(6) &&
                ECLookahead(1) == 'u' &&
                standardEncodedChars->IsHex(ECLookahead(2)) &&
                standardEncodedChars->IsHex(ECLookahead(3)) &&
                standardEncodedChars->IsHex(ECLookahead(4)) &&
                standardEncodedChars->IsHex(ECLookahead(5)))
            {
                if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                {
                    Fail(JSERR_RegExpSyntax);
                    return;
                }
                else
                {
                    uint32 n = (standardEncodedChars->DigitValue(ECLookahead(2)) << 12) |
                               (standardEncodedChars->DigitValue(ECLookahead(3)) << 8) |
                               (standardEncodedChars->DigitValue(ECLookahead(4)) << 4) |
                               (standardEncodedChars->DigitValue(ECLookahead(5)));
                    c = UTC(n);
                    consume = 6;
                }
            }
            else
            {
                c = Chars<EncodedChar>::CTW(ec);
                consume = 1;
            }

            switch (c) {
            case 'd':
                // ES2022 'd' flag: unconditional core syntax (no config flag)
                if ((flags & HasIndicesRegexFlag) != 0)
                {
                    Fail(JSERR_RegExpSyntax);
                }
                flags = (RegexFlags)(flags | HasIndicesRegexFlag);
                break;
            case 'i':
                if ((flags & IgnoreCaseRegexFlag) != 0)
                {
                    Fail(JSERR_RegExpSyntax);
                }
                flags = (RegexFlags)(flags | IgnoreCaseRegexFlag);
                break;
            case 'g':
                if ((flags & GlobalRegexFlag) != 0)
                {
                    Fail(JSERR_RegExpSyntax);
                }
                flags = (RegexFlags)(flags | GlobalRegexFlag);
                break;
            case 'm':
                if ((flags & MultilineRegexFlag) != 0)
                {
                    Fail(JSERR_RegExpSyntax);
                }
                flags = (RegexFlags)(flags | MultilineRegexFlag);
                break;
            case 's':
                if (scriptContext->GetConfig()->IsES2018RegExDotAllEnabled())
                {
                    if ((flags & DotAllRegexFlag) != 0)
                    {
                        Fail(JSERR_RegExpSyntax);
                    }
                    flags = (RegexFlags)(flags | DotAllRegexFlag);
                    break;
                }
            case 'u':
                // If we don't have unicode enabled, fall through to default
                if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                {
                    if ((flags & UnicodeRegexFlag) != 0)
                    {
                        Fail(JSERR_RegExpSyntax);
                    }
                    // ES2024: 'u' and 'v' are mutually exclusive.
                    if ((flags & UnicodeSetsRegexFlag) != 0)
                    {
                        Fail(JSERR_RegExpSyntax);
                    }
                    flags = (RegexFlags)(flags | UnicodeRegexFlag);
                    // For telemetry
                    CHAKRATEL_LANGSTATS_INC_LANGFEATURECOUNT(ES6, UnicodeRegexFlag, scriptContext);

                    break;
                }
            case 'v':
                // ES2024 'v' flag (Unicode sets): shares the unicode
                // configuration gate; u+v is rejected above and here.
                if (scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled())
                {
                    if ((flags & UnicodeSetsRegexFlag) != 0)
                    {
                        Fail(JSERR_RegExpSyntax);
                    }
                    if ((flags & UnicodeRegexFlag) != 0)
                    {
                        Fail(JSERR_RegExpSyntax);
                    }
                    flags = (RegexFlags)(flags | UnicodeSetsRegexFlag);
                    break;
                }
                // Without the unicode configuration the flag character is
                // invalid (it is a word character, like the default case).
                Fail(JSERR_RegExpSyntax);
                return;
            case 'y':
                if (scriptContext->GetConfig()->IsES6RegExStickyEnabled())
                {
                    if ((flags & StickyRegexFlag) != 0)
                    {
                        Fail(JSERR_RegExpSyntax);
                    }
                    flags = (RegexFlags)(flags | StickyRegexFlag);
                    // For telemetry
                    CHAKRATEL_LANGSTATS_INC_LANGFEATURECOUNT(ES6, StickyRegexFlag, scriptContext);

                    break;
                }
            default:
                if (standardChars->IsWord(c))
                {
                    // Outer context could never parse this character. Signal the syntax error as
                    // being part of the regex.
                    Fail(JSERR_RegExpSyntax);
                }
                return;
            }

            ECConsume(consume);
        }
    }

    //
    // Entry points
    //

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::ParseDynamic
        ( const EncodedChar* body
        , const EncodedChar* bodyLim
        , const EncodedChar* opts
        , const EncodedChar* optsLim
        , RegexFlags& flags )
    {
        Assert(!IsLiteral);
        Assert(body != 0);
        Assert(bodyLim >= body && *bodyLim == 0);
        Assert(opts == 0 || (optsLim >= opts && *optsLim == 0));

        // Body, pass 0
        SetPosition(body, bodyLim, true);

        PatternPass0();
        if (!IsEOF())
            Fail(JSERR_RegExpSyntax);

        // Options
        if (opts != 0)
        {
            SetPosition(opts, optsLim, false);
            Options(flags);
            if (!IsEOF())
                Fail(JSERR_RegExpSyntax);
            this->unicodeFlagPresent = (flags & (UnifiedRegex::UnicodeRegexFlag | UnifiedRegex::UnicodeSetsRegexFlag)) != 0;
            this->unicodeSetsFlagPresent = (flags & UnifiedRegex::UnicodeSetsRegexFlag) == UnifiedRegex::UnicodeSetsRegexFlag;
            this->caseInsensitiveFlagPresent = (flags & UnifiedRegex::IgnoreCaseRegexFlag) == UnifiedRegex::IgnoreCaseRegexFlag;
            this->dotAllFlagPresent = (flags & UnifiedRegex::DotAllRegexFlag) == UnifiedRegex::DotAllRegexFlag;
            this->multilineFlagPresent = (flags & UnifiedRegex::MultilineRegexFlag) == UnifiedRegex::MultilineRegexFlag;
            Assert(!this->unicodeFlagPresent || scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled());
            Assert(!this->dotAllFlagPresent || scriptContext->GetConfig()->IsES2018RegExDotAllEnabled());
        }
        else
        {
            this->unicodeFlagPresent = false;
            this->unicodeSetsFlagPresent = false;
            this->caseInsensitiveFlagPresent = false;
            this->dotAllFlagPresent = false;
            this->multilineFlagPresent = false;
        }

        // If this HR has been set, that means we have an earlier failure than the one caught above.
        if (this->deferredIfNotUnicodeError != nullptr && !this->unicodeFlagPresent)
        {
            throw ParseError(*deferredIfNotUnicodeError);
        }
        else if(this->deferredIfUnicodeError != nullptr && this->unicodeFlagPresent)
        {
            throw ParseError(*deferredIfUnicodeError);
        }
        else if (this->deferredIfUnicodeSetsError != nullptr && this->unicodeSetsFlagPresent)
        {
            throw ParseError(*this->deferredIfUnicodeSetsError);
        }
        else if (this->deferredIfUnicodeNoSetsError != nullptr && this->unicodeFlagPresent && !this->unicodeSetsFlagPresent)
        {
            throw ParseError(*this->deferredIfUnicodeNoSetsError);
        }

        // Decimal escapes need both the flag and the final group count.
        ValidateDecimalEscapesIfUnicode();

        // Named backreference resolution needs both the full set of group
        // definitions and the u flag, so it runs only now.
        ResolveNamedGroupRefs(body, bodyLim);

        this->currentSurrogatePairNode = this->surrogatePairList;

        // Body, pass 1
        SetPosition(body, bodyLim, true);
        Node* root = PatternPass1();
        Assert(IsEOF());


        return root;
    }

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::ParseLiteral
        ( const EncodedChar* input
        , const EncodedChar* inputLim
        , CharCount& outBodyEncodedChars
        , CharCount& outTotalEncodedChars
        , CharCount& outBodyChars
        , CharCount& outTotalChars
        , RegexFlags& flags )
    {
        Assert(IsLiteral);
        Assert(input != 0);
        Assert(inputLim >= input); // *inputLim need not be 0 because of deferred parsing

        // To handle surrogate pairs properly under unicode option, we will collect information on location of the pairs
        // during pass 0, regardless if the option is present. (We aren't able to get it at that time)
        // During pass 1, we will use that information to correctly create appropriate nodes.

        // Body, pass 0
        SetPosition(input, inputLim, true);

        PatternPass0();
        outBodyEncodedChars = Chars<EncodedChar>::OSB(next, input);
        outBodyChars = Pos();

        // Options are needed for the next pass
        ECMust('/', ERRnoSlash);
        Options(flags);
        this->unicodeFlagPresent = (flags & (UnifiedRegex::UnicodeRegexFlag | UnifiedRegex::UnicodeSetsRegexFlag)) != 0;
        this->unicodeSetsFlagPresent = (flags & UnifiedRegex::UnicodeSetsRegexFlag) == UnifiedRegex::UnicodeSetsRegexFlag;
        this->caseInsensitiveFlagPresent = (flags & UnifiedRegex::IgnoreCaseRegexFlag) == UnifiedRegex::IgnoreCaseRegexFlag;
        this->dotAllFlagPresent = (flags & UnifiedRegex::DotAllRegexFlag) == UnifiedRegex::DotAllRegexFlag;
        this->multilineFlagPresent = (flags & UnifiedRegex::MultilineRegexFlag) == UnifiedRegex::MultilineRegexFlag;
        Assert(!this->unicodeFlagPresent || scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled());

        // If this HR has been set, that means we have an earlier failure than the one caught above.
        if (this->deferredIfNotUnicodeError != nullptr && !this->unicodeFlagPresent)
        {
            throw ParseError(*deferredIfNotUnicodeError);
        }
        else if(this->deferredIfUnicodeError != nullptr && this->unicodeFlagPresent)
        {
            throw ParseError(*deferredIfUnicodeError);
        }
        else if (this->deferredIfUnicodeSetsError != nullptr && this->unicodeSetsFlagPresent)
        {
            throw ParseError(*this->deferredIfUnicodeSetsError);
        }
        else if (this->deferredIfUnicodeNoSetsError != nullptr && this->unicodeFlagPresent && !this->unicodeSetsFlagPresent)
        {
            throw ParseError(*this->deferredIfUnicodeNoSetsError);
        }

        // Decimal escapes need both the flag and the final group count.
        ValidateDecimalEscapesIfUnicode();

        // Named backreference resolution needs both the full set of group
        // definitions and the u flag, so it runs only now.
        ResolveNamedGroupRefs(input, inputLim);

        // Used below to proceed to the end of the regex
        const EncodedChar *pastOptions = next;

        this->currentSurrogatePairNode = this->surrogatePairList;

        // Body, pass 1
        SetPosition(input, inputLim, true);
        Node* root = PatternPass1();
        Assert(outBodyEncodedChars == Chars<EncodedChar>::OSB(next, input));
        Assert(outBodyChars == Pos());

        next = pastOptions;
        outTotalEncodedChars = Chars<EncodedChar>::OSB(next, input);
        outTotalChars = Pos();

        return root;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::ParseLiteralNoAST
        ( const EncodedChar* input
        , const EncodedChar* inputLim
        , CharCount& outBodyEncodedChars
        , CharCount& outTotalEncodedChars
        , CharCount& outBodyChars
        , CharCount& outTotalChars )
    {
        Assert(IsLiteral);
        Assert(input != 0);
        Assert(inputLim >= input); // *inputLim need not be 0 because of deferred parsing

        // Body, pass 0
        SetPosition(input, inputLim, true);
        PatternPass0();
        outBodyEncodedChars = Chars<EncodedChar>::OSB(next, input);
        outBodyChars = Pos();

        // Options
        ECMust('/', ERRnoSlash);
        RegexFlags dummyFlags = NoRegexFlags;
        Options(dummyFlags);
        this->unicodeFlagPresent = (dummyFlags & (UnifiedRegex::UnicodeRegexFlag | UnifiedRegex::UnicodeSetsRegexFlag)) != 0;
        this->unicodeSetsFlagPresent = (dummyFlags & UnifiedRegex::UnicodeSetsRegexFlag) == UnifiedRegex::UnicodeSetsRegexFlag;
        this->caseInsensitiveFlagPresent = (dummyFlags & UnifiedRegex::IgnoreCaseRegexFlag) == UnifiedRegex::IgnoreCaseRegexFlag;
        this->dotAllFlagPresent = (dummyFlags & UnifiedRegex::DotAllRegexFlag) == UnifiedRegex::DotAllRegexFlag;
        this->multilineFlagPresent = (dummyFlags & UnifiedRegex::MultilineRegexFlag) == UnifiedRegex::MultilineRegexFlag;
        outTotalEncodedChars = Chars<EncodedChar>::OSB(next, input);
        outTotalChars = Pos();

        // If this HR has been set, that means we have an earlier failure than the one caught above.
        if (this->deferredIfNotUnicodeError != nullptr && !this->unicodeFlagPresent)
        {
            throw ParseError(*deferredIfNotUnicodeError);
        }
        else if(this->deferredIfUnicodeError != nullptr && this->unicodeFlagPresent)
        {
            throw ParseError(*deferredIfUnicodeError);
        }
        else if (this->deferredIfUnicodeSetsError != nullptr && this->unicodeSetsFlagPresent)
        {
            throw ParseError(*this->deferredIfUnicodeSetsError);
        }
        else if (this->deferredIfUnicodeNoSetsError != nullptr && this->unicodeFlagPresent && !this->unicodeSetsFlagPresent)
        {
            throw ParseError(*this->deferredIfUnicodeNoSetsError);
        }

        // Decimal escapes need both the flag and the final group count;
        // this no-AST scan is the one deferred functions get, and a syntax
        // error discovered only on the later reparse is reported as an
        // internal error, so the validation has to run now too.
        ValidateDecimalEscapesIfUnicode();

        // ParseLiteral resolves named references here. This no-AST scan is
        // the one deferred functions get, and a syntax error discovered only
        // on the later reparse is reported as an internal error, so the
        // dangling-name and illegal class-\k checks have to run now too.
        // The call restores the position captured in outTotal* above.
        ResolveNamedGroupRefs(input, inputLim);
    }

    template <typename P, const bool IsLiteral>
    template <const bool buildAST>
    RegexPattern * Parser<P, IsLiteral>::CompileProgram
        ( Node* root,
          const EncodedChar*& currentCharacter,
          const CharCount totalLen,
          const CharCount bodyChars,
          const CharCount bodyEncodedChars,
          const CharCount totalChars,
          const RegexFlags flags )
    {
        Assert(IsLiteral);

        Program* program = nullptr;

        if (buildAST)
        {
            const auto recycler = this->scriptContext->GetRecycler();
            program = Program::New(recycler, flags);
            this->CaptureSourceAndGroups(recycler, program, currentCharacter, bodyChars, bodyEncodedChars);
        }

        currentCharacter += totalLen;
        Assert(GetMultiUnits() == totalLen - totalChars);

        if (!buildAST)
        {
            return nullptr;
        }

        RegexPattern* pattern = RegexPattern::New(this->scriptContext, program, true);

#if ENABLE_REGEX_CONFIG_OPTIONS
        RegexStats* stats = 0;
        if (REGEX_CONFIG_FLAG(RegexProfile))
        {
            stats = this->scriptContext->GetRegexStatsDatabase()->GetRegexStats(pattern);
            this->scriptContext->GetRegexStatsDatabase()->EndProfile(stats, RegexStats::Parse);
        }
        if (REGEX_CONFIG_FLAG(RegexTracing))
        {
            DebugWriter* tw = this->scriptContext->GetRegexDebugWriter();
            tw->Print(_u("// REGEX COMPILE "));
            pattern->Print(tw);
            tw->EOL();
        }
        if (REGEX_CONFIG_FLAG(RegexProfile))
            this->scriptContext->GetRegexStatsDatabase()->BeginProfile();
#endif

        ArenaAllocator* rtAllocator = this->scriptContext->RegexAllocator();
        Compiler::Compile
            ( this->scriptContext
              , ctAllocator
              , rtAllocator
              , standardChars
              , program
              , root
              , this->GetLitbuf()
              , pattern
#if ENABLE_REGEX_CONFIG_OPTIONS
              , w
              , stats
#endif
                );

#if ENABLE_REGEX_CONFIG_OPTIONS
        if (REGEX_CONFIG_FLAG(RegexProfile))
            this->scriptContext->GetRegexStatsDatabase()->EndProfile(stats, RegexStats::Compile);
#endif

#ifdef PROFILE_EXEC
        this->scriptContext->ProfileEnd(Js::RegexCompilePhase);
#endif
        // CaptureSourceAndGroups throws if this condition doesn't hold.
        Assert(0 < pattern->NumGroups() && pattern->NumGroups() <= MAX_NUM_GROUPS);

        return pattern;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::CaptureEmptySourceAndNoGroups(Program* program)
    {
        Assert(program->source == 0);

        program->source = const_cast<Char*>(_u(""));
        program->sourceLen = 0;

        program->numGroups = 1;

        // Remaining to set during compilation: litbuf, litbufLen, numLoops, insts, instsLen, entryPointLabel
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::CaptureSourceAndGroups(Recycler* recycler, Program* program, const EncodedChar* body, CharCount bodyChars, CharCount bodyEncodedChars)
    {
        Assert(program->source == 0);
        Assert(body != 0);

        // Program will own source string
        program->source = RecyclerNewArrayLeaf(recycler, Char, bodyChars + 1);
        // Don't need to zero out since we're writing to the buffer right here
        this->ConvertToUnicode(program->source, bodyChars, body, body + bodyEncodedChars);

        program->source[bodyChars] = 0;
        program->sourceLen = bodyChars;

        // We expect nextGroupId to be positive, because the full regexp itself always
        // counts as a capturing group.
        Assert(nextGroupId > 0);
        if (nextGroupId > MAX_NUM_GROUPS)
        {
            Js::JavascriptError::ThrowRangeError(this->scriptContext, JSERR_RegExpTooManyCapturingGroups);
        }
        else
        {
            program->numGroups = static_cast<uint16>(nextGroupId);
        }

        program->hasScopedModifiers = this->hasScopedModifiers;
        CaptureGroupNames(recycler, program);

        // Remaining to set during compilation: litbuf, litbufLen, numLoops, insts, instsLen, entryPointLabel
    }

    template <typename P, const bool IsLiteral>
    Node* Parser<P, IsLiteral>::GetNodeWithValidCharacterSet(EncodedChar cc)
    {
        Node* nodeToReturn = nullptr;
        if (this->scriptContext->GetConfig()->IsES6UnicodeExtensionsEnabled() && this->unicodeFlagPresent)
        {
            // One deterministic whole-code-point-set atom: the same standard
            // set the previous BMP prefix arm carried (lone-surrogate code
            // points included as members), lifted to code points with the
            // astral planes 0x10000..0x10FFFF added. The previous
            // prefix/pair/lone-high-surrogate alternation left a complete
            // surrogate pair with two live interpretations (the 2-unit pair
            // arm and a 1-unit lone-high arm), so a quantified consumer such
            // as (.+).*\1/u could capture a half of a pair and backtrack
            // into it; TranslateCodePointSet's matcher resolves each
            // position to exactly one interpretation.
            CharSet<Char> units;
            switch (cc)
            {
            case '.':
                if (this->dotAllFlagPresent)
                {
                    standardChars->SetFullSet(ctAllocator, units);
                }
                else
                {
                    standardChars->SetNonNewline(ctAllocator, units);
                }
                break;
            case 'S':
                standardChars->SetNonWhitespace(ctAllocator, units);
                break;
            case 'D':
                standardChars->SetNonDigits(ctAllocator, units);
                break;
            case 'W':
                if (this->caseInsensitiveFlagPresent)
                {
                    standardChars->SetNonWordIUChars(ctAllocator, units);
                }
                else
                {
                    standardChars->SetNonWordChars(ctAllocator, units);
                }
                break;
            default:
                AssertMsg(false, "");
            }

            CharSet<codepoint_t> codePointSet;
            Char lo = 0, hi = 0;
            Char search = 0;
            while (units.GetNextRange(search, &lo, &hi))
            {
                codePointSet.SetRange(ctAllocator, lo, hi);
                if (hi == Chars<Char>::MaxUChar)
                    break;
                search = hi + 1;
            }
            units.FreeBody(ctAllocator);
            codePointSet.SetRange(ctAllocator, 0x10000, 0x10FFFF);
            nodeToReturn = TranslateCodePointSet(codePointSet, false);
        }
        else
        {
            MatchSetNode* setNode = Anew(ctAllocator, MatchSetNode, false, false, caseInsensitiveFlagPresent);
            switch (cc)
            {
            case '.':
                if (this->dotAllFlagPresent)
                {
                    standardChars->SetFullSet(ctAllocator, setNode->set);
                }
                else
                {
                    standardChars->SetNonNewline(ctAllocator, setNode->set);
                }
                break;
            case 'S':
                standardChars->SetNonWhitespace(ctAllocator, setNode->set);
                break;
            case 'D':
                standardChars->SetNonDigits(ctAllocator, setNode->set);
                break;
            case 'W':
                standardChars->SetNonWordChars(ctAllocator, setNode->set);
                break;
            default:
                AssertMsg(false, "");
            }
            nodeToReturn = setNode;
        }

        return nodeToReturn;
    }

    template <typename P, const bool IsLiteral>
    void Parser<P, IsLiteral>::FreeBody()
    {
        if (litbuf != 0)
        {
            ctAllocator->Free(litbuf, litbufLen);
            litbuf = nullptr;
            litbufLen = 0;
            litbufNext = 0;
        }
    }

    // Instantiate all templates
#define INSTANTIATE_REGEX_PARSER_COMPILE(EncodingPolicy, IsLiteral, BuildAST)    \
    template RegexPattern* Parser<EncodingPolicy, IsLiteral>::CompileProgram<BuildAST>(Node* root, const EncodedChar*& currentCharacter, const CharCount totalLen, const CharCount bodyChars, const CharCount bodyEncodedChars, const CharCount totalChars, const RegexFlags flags );

#define INSTANTIATE_REGEX_PARSER(EncodingPolicy)                     \
    INSTANTIATE_REGEX_PARSER_COMPILE(EncodingPolicy, false, false)   \
    INSTANTIATE_REGEX_PARSER_COMPILE(EncodingPolicy, false, true)    \
    INSTANTIATE_REGEX_PARSER_COMPILE(EncodingPolicy, true, false)    \
    INSTANTIATE_REGEX_PARSER_COMPILE(EncodingPolicy, true, true)     \
    template class Parser<EncodingPolicy, false>;                    \
    template class Parser<EncodingPolicy, true>;

    // Instantiate the Parser
    INSTANTIATE_REGEX_PARSER(NullTerminatedUnicodeEncodingPolicy);
    INSTANTIATE_REGEX_PARSER(NotNullTerminatedUTF8EncodingPolicy);
}
