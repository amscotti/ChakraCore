//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace UnifiedRegex
{
    struct ParseError
    {
        bool isBody;
        CharCount pos;           // Position in unicode characters
        CharCount encodedPos;    // Position in underlying characters (eg utf-8 bytes)
        HRESULT error;

        ParseError(bool isBody, CharCount pos, CharCount encodedPos, HRESULT error);
    };

    template <typename EncodingPolicy, const bool IsLiteral>
    class Parser : private EncodingPolicy, private Chars<char16>
    {
    private:
        typedef typename EncodingPolicy::EncodedChar EncodedChar;

        // A linked list node to track indices of surrogate pairs.
        struct SurrogatePairTracker
        {
            const EncodedChar* location;
            // If this surrogate pair is inside a range, then rangeLocation isn't null.
            const EncodedChar* rangeLocation;
            codepoint_t value;
            uint32 length;
            size_t multiUnits;
            SurrogatePairTracker* next;

            SurrogatePairTracker(const EncodedChar* location, codepoint_t value, uint32 length, size_t multiUnits)
                : location(location)
                , next(nullptr)
                , value(value)
                , length(length)
                , multiUnits(multiUnits)
                , rangeLocation(nullptr)
            {
            }

            SurrogatePairTracker(const EncodedChar* location, const EncodedChar* rangeLocation, codepoint_t value, uint32 length, size_t multiUnits)
                : location(location)
                , next(nullptr)
                , value(value)
                , length(length)
                , multiUnits(multiUnits)
                , rangeLocation(rangeLocation)
            {
            }

            bool IsInsideRange() const
            {
                return this->rangeLocation != nullptr;
            }
        };

        static const CharCount initLitbufSize = 16;

        // A named capture group definition (recorded during pass 0), or an
        // unresolved \k<name> reference (groupId unused). Names are decoded
        // UTF-16 sequences appended to namedGroupBuffer. ES2025 allows a name
        // to be bound by several groups, but only when a disjunction separates
        // them (see AltScope below), so one alternative path never passes
        // through two groups of the same name.
        struct NamedGroup
        {
            CharCount offset; // offset into namedGroupBuffer
            CharCount len;    // length in Char units
            int groupId;      // definition only: capture index, 1-based
        };

        // Pass-0 alternative-scope state for the duplicate-name rule. The
        // stack altPathGroupIdx holds indices into namedGroups for every
        // definition reachable along the current alternative path:
        // [altFrameStart, altPathCount) are the current alternative's names
        // while the region below (from the enclosing scope's savedAccStart)
        // is the accumulator of the alternatives already parsed in this
        // disjunction. Each open disjunction pushes one AltScope record;
        // its alternatives start fresh frames, '|' folds a finished frame
        // into the accumulator, and closing the disjunction checks the
        // whole accumulator against the enclosing frame (a merge there
        // would put both same-name groups on one path) and folds it in.
        struct AltScope
        {
            int savedFrameStart; // enclosing alternative's frame start
            int savedAccStart;   // this disjunction's accumulator start
        };

        // Raw span of a \k<...> reference, recorded during pass 0 in encoded
        // characters. Whether the escape is a backreference or an Annex B
        // identity escape depends on the whole pattern (forward references)
        // and the u flag, so the span is validated only at resolution time.
        struct NamedGroupRefSpan
        {
            const EncodedChar* nameStart; // first char after '\k<'
            const EncodedChar* afterGt;   // one past '>', or null when unterminated
        };

        NamedGroup* namedGroups;        // definitions, in textual order (ctAllocator)
        int numNamedGroups;
        int namedGroupsCap;
        NamedGroupRefSpan* namedGroupRefSpans; // \k<...> occurrences (ctAllocator)
        int numNamedGroupRefSpans;
        int namedGroupRefSpansCap;
        Char* namedGroupBuffer;      // decoded names, concatenated (ctAllocator)
        CharCount namedGroupBufferLen;  // end of the pass 0 region
        CharCount namedGroupBufferNext; // append cursor (pass 1/resolution append past ^)
        CharCount namedGroupBufferCap;
        bool sawBareK;               // '\k' not followed by '<' anywhere
        bool sawClassK;              // '\k' inside a character class anywhere
        int* altPathGroupIdx;        // namedGroups indices on the current path (ctAllocator)
        int altPathCount;
        int altPathCap;
        AltScope* altScopes;         // open disjunction scopes, innermost last (ctAllocator)
        int altScopeCount;
        int altScopeCap;
        int altFrameStart;           // start of the current alternative's frame

        Js::ScriptContext* scriptContext;
        // Arena for nodes and items needed only during compilation
        ArenaAllocator* ctAllocator;
        // Standard characters using raw encoding character representation (eg char for utf-8)
        StandardChars<EncodedChar>* standardEncodedChars;
        // Standard characters using final character representation (eg char16 for Unicode)
        StandardChars<Char>* standardChars;
#if ENABLE_REGEX_CONFIG_OPTIONS
        DebugWriter* w;
#endif

        const EncodedChar* input;
        const EncodedChar* inputLim;
        const EncodedChar* next;
        bool inBody;

        // Maximum number of capturing groups allowed, including the entire regexp, which is always
        // considered a capturing group.  Using INT16_MAX allows us to pass one value for each
        // group, plus a few additional values, to a JavaScript function without overflowing the
        // number of arguments.  This is important, for example, in the implementation of
        // String.prototype.replace, where the second argument is a function.
        static const uint16 MAX_NUM_GROUPS = INT16_MAX;

        uint16 numGroups; // determined in first parse

        // Keeps track of how many capturing groups we've seen during parsing.  We use an int, rather than
        // a uint16, to be sure we don't overflow during parsing and only check it against MAX_NUM_GROUPS at
        // the end.  (We know we can't overflow an int because strings and regex literals are limited to
        // 2G characters and therefore to 1G pairs of parentheses, which can fit into an int.  I'd prefer
        // to use size_t here, but making that change would go down a serious rabbit hole changing the
        // interface to UnifiedRegex::Node::AccumDefineGroups.)
        int nextGroupId;

        // Buffer accumulating all literals.
        // In compile-time allocator, must be transferred to runtime allocator when build program
        Char* litbuf;
        CharCount litbufLen;
        CharCount litbufNext;

        // During pass 0, if /u option for regex is provided, a linked list will be built up to
        // track positions of surrogate pairs in the buffer. During pass 1, these linked lists will be used
        // to figure out when to output a surrogate pair node.
        SurrogatePairTracker* surrogatePairList;
        SurrogatePairTracker* currentSurrogatePairNode;
        bool unicodeFlagPresent;
        bool caseInsensitiveFlagPresent;
        bool dotAllFlagPresent;
        bool multilineFlagPresent;
        // True once pass 1 parses an ES2025 modifier group (?ims-ims:...).
        // Copied to Program::hasScopedModifiers so the compiler can disable
        // whole-program special cases that assume top-level flags.
        bool hasScopedModifiers;

        // The following two variables are used to determine if the the surrogate pair has been encountered
        // First holds the temporary location, second holds the value of the codepoint
        const EncodedChar* tempLocationOfSurrogatePair;
        // This will be set to a location when we are parsing a range in TermPass0, and cleared when we are out of it.
        const EncodedChar* tempLocationOfRange;
        codepoint_t codePointAtTempLocation;

        // When a surrogate is added for tracking, this will be updated.
        const EncodedChar* positionAfterLastSurrogate;
        codepoint_t valueOfLastSurrogate;

        // deferred error state.
        ParseError* deferredIfNotUnicodeError;
        ParseError* deferredIfUnicodeError;

    private:

        //
        // Input buffer management
        //

        void SetPosition(const EncodedChar* input, const EncodedChar* inputLim, bool inBody);

        // Current position in number of logical characters, regardless of underlying character encoding
        inline CharCount Pos();

        inline bool IsEOF();
        inline bool ECCanConsume(CharCount n = 1);
        inline EncodedChar ECLookahead(CharCount n = 0);
        inline EncodedChar ECLookback(CharCount n = 0);
        inline void ECConsume(CharCount n = 1);
        inline void ECConsumeMultiUnit(CharCount n = 1);
        inline void ECRevert(CharCount n = 1);

        //
        // Helpers
        //
        int TryParseExtendedUnicodeEscape(Char& c, bool& previousSurrogatePart, bool trackSurrogatePair = false);
        void TrackIfSurrogatePair(codepoint_t codePoint, const EncodedChar* location, uint32 consumptionLength);
        Node* CreateSurrogatePairAtom(char16 lower, char16 upper);
        AltNode* AppendSurrogateRangeToDisjunction(codepoint_t lowerCodePoint, codepoint_t upperCodePoint, AltNode *lastAlttNode);
        AltNode* AppendSurrogatePairToDisjunction(codepoint_t codePoint, AltNode *lastAlttNode);

        // Raw span of a \p{...}/\P{...} escape, recorded during pass 0. The
        // u flag is unknown in pass 0, so the body is only scanned to the
        // closing '}' (unterminated -> deferred-if-unicode error) and its
        // names are validated with deferred-if-unicode errors; pass 1
        // re-parses the same span and builds the node (u mode only). Without
        // the u flag the whole escape re-parses as the Annex B identity
        // escape 'p'/'P' plus literal '{...}' characters, as before.

        // Longest property/value spelling ("Changes_When_NFKC_Casefolded"
        // and friends); longer bodies are invalid names, so scanning can
        // stop collecting and just mark the span invalid.
        static const CharCount propertyEscapeMaxNameLen = 64;

        // Cursor at 'p' or 'P' (the '\' is consumed). Consumes through the
        // closing '}' when the body is structurally terminated, records the
        // span, and validates the name with a deferred-if-unicode error.
        void ScanPropertyEscapePass0();
        // Cursor at '{' ('p'/'P' already consumed by either the atom path
        // above or the class-escape switch): the shared pass 0 body scan and
        // deferred-if-unicode validation.
        void ScanPropertyEscapeBodyPass0();
        // Cursor at '{' (p/P consumed). Consumes through '}' and reduces the
        // body to ASCII name/value buffers; false when the body cannot be a
        // valid property expression. Unterminated bodies Fail.
        bool ScanPropertyEscapeBody(char16* name, CharCount& nameLen,
                                    char16* value, CharCount& valueLen, bool& hasValue);
        // Cursor at 'p' or 'P', u mode: consumes the escape and returns the
        // matching node (code-point-set translation below). Never an
        // assertion.
        Node* ParsePropertyEscapePass1();
        // Cursor at 'p' or 'P', u mode, inside a character class: consumes
        // the escape and unions its code point set into targetSet (\P{...}
        // contributes its complement).
        void UnionPropertyEscapePass1(CharSet<codepoint_t>& targetSet);
        // Translates a finished (complemented / case-closed) code point set
        // into matcher nodes: a simple character or surrogate-pair atom when
        // singleton, a single MatchSetNode when all-BMP, otherwise a
        // disjunction of the BMP set and surrogate-pair range arms. Shared
        // by character classes and property escapes.
        Node* TranslateCodePointSet(CharSet<codepoint_t>& codePointSet, bool isNegation);

        //
        // Errors
        //

        void Fail(HRESULT error);
        void DeferredFailIfUnicode(HRESULT error);
        void DeferredFailIfNotUnicode(HRESULT error);
        inline void ECMust(EncodedChar ec, HRESULT error);
        inline Char NextChar();

        //
        // Named capture groups (ES2018)
        //

        void EnsureNameBuffer(CharCount additionalChars);
        void AppendNameCodePoint(codepoint_t codePoint);
        codepoint_t ScanNameUnicodeEscape();
        void ScanGroupName();
        void RecordNamedGroup(const Char* name, CharCount len, int groupId);
        int FindNamedGroup(const Char* name, CharCount len) const;
        bool NamedGroupIsAt(const NamedGroup& group, const Char* name, CharCount len) const;
        void EnsureAltPathCapacity();
        void EnsureAltScopeCapacity();
        void PushAltScope();
        void FoldAlternativeIntoScope();
        void PopAltScope();
        void ResolveNamedGroupRefs(const EncodedChar* body, const EncodedChar* bodyLim);
        void CaptureGroupNames(Recycler* recycler, Program* program);

        //
        // Lookbehind assertions (ES2018)
        //

        // > 0 while parsing anywhere inside a lookbehind, including inside
        // nested lookaheads (whose bodies then also run in a non-top frame).
        // Anchors and word boundaries parsed while this is non-zero are
        // frame-aware, and the mirroring pass below applies to lookbehind
        // bodies.
        int lookbehindDepth;
        // Syntactic mirroring of a lookbehind body: reverses term order (and
        // literal characters) recursively through concatenations, groups,
        // loops and alternations (alternatives keep textual order), and
        // re-orients nested assertions (lookahead <-> lookbehind; see
        // LookbehindNode). The result is a pattern that, matched forward over
        // a reversed window, is exactly the spec's right-to-left match of the
        // original body.
        Node* MirrorLookbehindBody(Node* body);
        Node* MirrorLiteral(const MatchLiteralNode* lit);
        // Recognizes the parser's synthetic surrogate-pair atoms (see the
        // definition in RegexParser.cpp); u-mode mirroring keeps their two
        // units in order.
        bool IsSurrogatePairConcat(Node* node);

        //
        // Patterns/Disjunctions/Alternatives
        //

        void PatternPass0();
        Node* PatternPass1();
        Node* UnionNodes(Node* prev, Node* curr);
        void DisjunctionPass0(int depth);
        Node* DisjunctionPass1();
        bool IsEndOfAlternative();
        void EnsureLitbuf(CharCount size);
        void AccumLiteral(MatchLiteralNode* deferredLiteralNode, Node* charOrLiteralNode);
        Node* FinalTerm(Node* node, MatchLiteralNode* deferredLiteralNode);
        void AlternativePass0(int depth);
        Node* AlternativePass1();

        //
        // Terms
        //

        Node* NewLoopNode(CharCount lower, CharCountOrFlag upper, bool isGreedy, Node* body);
        bool AtQuantifier();
        bool OptNonGreedy();
        CharCount RepeatCount();
        // ES2025 scoped RegExp modifiers: recognizes (?ims-ims: with the
        // cursor at the '?' that follows '(' (callers have consumed '('),
        // validates it, and consumes through ':'. Returns false when the
        // text is not a modifier group (so the ordinary group/capture
        // handling continues). Once the first character says modifier
        // (i, m, s or '-'), malformed input is a SyntaxError.
        bool TryParseModifierGroup(RegexFlags& addFlags, RegexFlags& removeFlags);
        void TermPass0(int depth);
        Node* TermPass1(MatchCharNode* deferredCharNode, bool& previousSurrogatePart);
        bool AtomEscapePass0();
        bool AtomEscapePass1(Node*& node, MatchCharNode* deferredCharNode, bool& previousSurrogatePart);
        bool SurrogatePairPass1(Node*& node, MatchCharNode* deferredCharNode, bool& previousSurrogatePart);

        //
        // Classes
        //

        bool AtSecondSingletonClassAtom();
        void CharacterClassPass0();
        template <bool containsSurrogates>
        Node* CharacterClassPass1();
        bool ClassEscapePass0(Char& singleton, bool& previousSurrogatePart);
        Node* ClassEscapePass1(MatchCharNode* deferredCharNode, MatchSetNode* deferredSetNode, bool& previousSurrogatePart);
        Node* GetNodeWithValidCharacterSet(EncodedChar ch);

        //
        // Options
        //

        void Options(RegexFlags& flags);

    public:

        Parser
            ( Js::ScriptContext* scriptContext
            , ArenaAllocator* ctAllocator
            , StandardChars<EncodedChar>* standardEncodedChars
            , StandardChars<Char>* standardChars
            , bool isUtf8
#if ENABLE_REGEX_CONFIG_OPTIONS
            , DebugWriter* w
#endif
            );

        //
        // Entry points
        //


        Node* ParseDynamic
            ( const EncodedChar* body           // non null, null terminated (may contain embedded nulls)
            , const EncodedChar* bodyLim        // points to terminating null of above
            , const EncodedChar* opts           // may be null if no options, otherwise null terminated
            , const EncodedChar* optsLim        // if above non-null, points to terminating null of above
            , RegexFlags& flags );

        // (*) For ParseLiteral:
        //  - input string must be null terminated
        //  - inputLim may point to the terminating null in above or before it
        //     - if the later, input is known to be syntactically well-formed so that the parser
        //       will find the natural end of the regex literal before passing inputLim
        //  - input may contain nulls before the inputLim

        Node* ParseLiteral
            ( const EncodedChar* input          // non null, null terminated (may contain embedded nulls)
            , const EncodedChar* inputLim       // see (*) above
            , CharCount& outBodyEncodedChars    // in encoded characters, not including trailing '/'
            , CharCount& outTotalEncodedChars   // in encoded characters, including trailing '/' and any options
            , CharCount& outBodyChars           // in unicode characters, not including ttrailing '/'
            , CharCount& outTotalChars          // in unicode characters, including trailing '/' and any options
            , RegexFlags& flags );

        void ParseLiteralNoAST
            ( const EncodedChar* input          // non null, null terminated
            , const EncodedChar* inputLim       // see (*) above
            , CharCount& outBodyEncodedChars
            , CharCount& outTotalEncodedChars
            , CharCount& outBodyChars
            , CharCount& outTotalChars );

        template<const bool buildAST>
        RegexPattern* CompileProgram
            ( Node* root,
              const EncodedChar*& currentCharacter,
              const CharCount totalLen,
              const CharCount bodyChars,
              const CharCount bodyEncodedChars,
              const CharCount totalChars,
              const RegexFlags flags );

        static void CaptureEmptySourceAndNoGroups(Program* program);

        // bodyChars is number of unicode characters in program body, which may be less than the number
        // of underlying UTF-8 characters
        void CaptureSourceAndGroups(Recycler* recycler, Program* program, const EncodedChar* body, CharCount bodyChars, CharCount bodyEncodedChars);

        inline const Char* GetLitbuf() { return litbuf; }

        void FreeBody();

        size_t GetMultiUnits() { return this->m_cMultiUnits; }
    };
}
