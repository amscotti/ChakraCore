//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ECMAScript Unicode property escapes (\p{...}/\P{...} under the u flag):
// name resolution against the ECMAScript property/alias tables and code point
// set construction through the ICU C APIs. No Unicode property data is
// embedded here; ICU 79.1 (the pinned dependency) owns all of it.
//
// Name matching is exact and case-sensitive (ES2024 removed the earlier
// loose matching): a spelling must be one of the canonical names or aliases.
// The ES property set is a strict subset of UCD/ICU properties (Block=, Age=,
// Hyphen, Other_*, Grapheme_Link, ... are rejected), so resolution uses
// allow-lists rather than ICU's loose u_getPropertyEnum lookup.
#pragma once

#include "RuntimeCommon.h"

namespace UnifiedRegex
{
    namespace UnicodeProperty
    {
        // What a resolved \p{...} body denotes:
        //  - BinaryProperty: value is a UProperty binary property enum
        //  - GeneralCategory: value is a UCharCategory, or (isGroupValue) a
        //    U_GC_*_MASK grouped-category mask
        //  - Script/ScriptExtensions: value is a UScriptCode
        //  - Any/ASCII/Assigned: the three RegExp-specific properties
        enum Kind : uint8
        {
            Invalid,
            BinaryProperty,
            GeneralCategory,
            Script,
            ScriptExtensions,
            Any,
            ASCII,
            Assigned
        };

        struct Resolved
        {
            Kind kind;
            int32 value;
            bool isGroupValue;
        };

        // Resolves one \p{...} body. `name` is the text before the first '='
        // (the whole body when `hasValue` is false), `value` the text after;
        // both must already be reduced to ASCII. Returns false when the body
        // is not a valid property expression (the caller turns that into a
        // SyntaxError under the u flag).
        bool Resolve(const char16* name, CharCount nameLen, const char16* value, CharCount valueLen, bool hasValue, Resolved* out);

        // Computes the matching code points into `result`, allocated from
        // `allocator`. Requires a successfully resolved escape.
        void BuildCodePointSet(const Resolved& resolved, ArenaAllocator* allocator, CharSet<codepoint_t>& result);

        // ---------------------------------------------------------------------------
        // Implementation (this header is consumed only by RegexParser.cpp; all
        // property data comes from ICU 79.1). A USet opened in
        // BuildCodePointSet is closed before returning; no ICU object or
        // native pointer outlives the call, and the CharSet follows the regex
        // compiler's existing arena ownership (ctAllocator during parsing,
        // cloned into the runtime allocator by the node transfer passes).
        // ---------------------------------------------------------------------------

#include <unicode/uchar.h>
#include <unicode/uscript.h>
#include <unicode/uset.h>

        // ES2024 binary property names and aliases (exact spellings). The
        // RegExp-specific Any/Assigned/ASCII are resolved separately.
        struct BinaryName { const char16* name; int32 prop; };
        static const BinaryName binaryNames[] =
        {
            { _u("Alphabetic"),                       UCHAR_ALPHABETIC },
            { _u("Alpha"),                            UCHAR_ALPHABETIC },
            { _u("ASCII_Hex_Digit"),                  UCHAR_ASCII_HEX_DIGIT },
            { _u("AHex"),                             UCHAR_ASCII_HEX_DIGIT },
            { _u("Bidi_Control"),                     UCHAR_BIDI_CONTROL },
            { _u("Bidi_C"),                           UCHAR_BIDI_CONTROL },
            { _u("Bidi_Mirrored"),                    UCHAR_BIDI_MIRRORED },
            { _u("Bidi_M"),                           UCHAR_BIDI_MIRRORED },
            { _u("Case_Ignorable"),                   UCHAR_CASE_IGNORABLE },
            { _u("CI"),                               UCHAR_CASE_IGNORABLE },
            { _u("Cased"),                            UCHAR_CASED },
            { _u("Changes_When_Casefolded"),          UCHAR_CHANGES_WHEN_CASEFOLDED },
            { _u("CWCF"),                             UCHAR_CHANGES_WHEN_CASEFOLDED },
            { _u("Changes_When_Casemapped"),          UCHAR_CHANGES_WHEN_CASEMAPPED },
            { _u("CWCM"),                             UCHAR_CHANGES_WHEN_CASEMAPPED },
            { _u("Changes_When_Lowercased"),          UCHAR_CHANGES_WHEN_LOWERCASED },
            { _u("CWL"),                              UCHAR_CHANGES_WHEN_LOWERCASED },
            { _u("Changes_When_NFKC_Casefolded"),     UCHAR_CHANGES_WHEN_NFKC_CASEFOLDED },
            { _u("CWKCF"),                            UCHAR_CHANGES_WHEN_NFKC_CASEFOLDED },
            { _u("Changes_When_Titlecased"),          UCHAR_CHANGES_WHEN_TITLECASED },
            { _u("CWT"),                              UCHAR_CHANGES_WHEN_TITLECASED },
            { _u("Changes_When_Uppercased"),          UCHAR_CHANGES_WHEN_UPPERCASED },
            { _u("CWU"),                              UCHAR_CHANGES_WHEN_UPPERCASED },
            { _u("Dash"),                             UCHAR_DASH },
            { _u("Default_Ignorable_Code_Point"),     UCHAR_DEFAULT_IGNORABLE_CODE_POINT },
            { _u("DI"),                               UCHAR_DEFAULT_IGNORABLE_CODE_POINT },
            { _u("Deprecated"),                       UCHAR_DEPRECATED },
            { _u("Dep"),                              UCHAR_DEPRECATED },
            { _u("Diacritic"),                        UCHAR_DIACRITIC },
            { _u("Dia"),                              UCHAR_DIACRITIC },
            { _u("Emoji"),                            UCHAR_EMOJI },
            { _u("Emoji_Component"),                  UCHAR_EMOJI_COMPONENT },
            { _u("EComp"),                            UCHAR_EMOJI_COMPONENT },
            { _u("Emoji_Modifier"),                   UCHAR_EMOJI_MODIFIER },
            { _u("EMod"),                             UCHAR_EMOJI_MODIFIER },
            { _u("Emoji_Modifier_Base"),              UCHAR_EMOJI_MODIFIER_BASE },
            { _u("EBase"),                            UCHAR_EMOJI_MODIFIER_BASE },
            { _u("Emoji_Presentation"),               UCHAR_EMOJI_PRESENTATION },
            { _u("EPres"),                            UCHAR_EMOJI_PRESENTATION },
            { _u("Extended_Pictographic"),            UCHAR_EXTENDED_PICTOGRAPHIC },
            { _u("ExtPict"),                          UCHAR_EXTENDED_PICTOGRAPHIC },
            { _u("Extender"),                         UCHAR_EXTENDER },
            { _u("Ext"),                              UCHAR_EXTENDER },
            { _u("Grapheme_Base"),                    UCHAR_GRAPHEME_BASE },
            { _u("Gr_Base"),                          UCHAR_GRAPHEME_BASE },
            { _u("Grapheme_Extend"),                  UCHAR_GRAPHEME_EXTEND },
            { _u("Gr_Ext"),                           UCHAR_GRAPHEME_EXTEND },
            { _u("Hex_Digit"),                        UCHAR_HEX_DIGIT },
            { _u("Hex"),                              UCHAR_HEX_DIGIT },
            { _u("IDS_Binary_Operator"),              UCHAR_IDS_BINARY_OPERATOR },
            { _u("IDSB"),                             UCHAR_IDS_BINARY_OPERATOR },
            { _u("IDS_Trinary_Operator"),             UCHAR_IDS_TRINARY_OPERATOR },
            { _u("IDST"),                             UCHAR_IDS_TRINARY_OPERATOR },
            { _u("ID_Continue"),                      UCHAR_ID_CONTINUE },
            { _u("IDC"),                              UCHAR_ID_CONTINUE },
            { _u("ID_Start"),                         UCHAR_ID_START },
            { _u("IDS"),                              UCHAR_ID_START },
            { _u("Ideographic"),                      UCHAR_IDEOGRAPHIC },
            { _u("Ideo"),                             UCHAR_IDEOGRAPHIC },
            { _u("Join_Control"),                     UCHAR_JOIN_CONTROL },
            { _u("Join_C"),                           UCHAR_JOIN_CONTROL },
            { _u("Logical_Order_Exception"),          UCHAR_LOGICAL_ORDER_EXCEPTION },
            { _u("LOE"),                              UCHAR_LOGICAL_ORDER_EXCEPTION },
            { _u("Lowercase"),                        UCHAR_LOWERCASE },
            { _u("Lower"),                            UCHAR_LOWERCASE },
            { _u("Math"),                             UCHAR_MATH },
            { _u("Noncharacter_Code_Point"),          UCHAR_NONCHARACTER_CODE_POINT },
            { _u("NChar"),                            UCHAR_NONCHARACTER_CODE_POINT },
            { _u("Pattern_Syntax"),                   UCHAR_PATTERN_SYNTAX },
            { _u("Pat_Syn"),                          UCHAR_PATTERN_SYNTAX },
            { _u("Pattern_White_Space"),              UCHAR_PATTERN_WHITE_SPACE },
            { _u("Pat_WS"),                           UCHAR_PATTERN_WHITE_SPACE },
            { _u("Quotation_Mark"),                   UCHAR_QUOTATION_MARK },
            { _u("QMark"),                            UCHAR_QUOTATION_MARK },
            { _u("Radical"),                          UCHAR_RADICAL },
            { _u("Regional_Indicator"),               UCHAR_REGIONAL_INDICATOR },
            { _u("RI"),                               UCHAR_REGIONAL_INDICATOR },
            { _u("Sentence_Terminal"),                UCHAR_S_TERM },
            { _u("STerm"),                            UCHAR_S_TERM },
            { _u("Soft_Dotted"),                      UCHAR_SOFT_DOTTED },
            { _u("SD"),                               UCHAR_SOFT_DOTTED },
            { _u("Terminal_Punctuation"),             UCHAR_TERMINAL_PUNCTUATION },
            { _u("Term"),                             UCHAR_TERMINAL_PUNCTUATION },
            { _u("Unified_Ideograph"),                UCHAR_UNIFIED_IDEOGRAPH },
            { _u("UIdeo"),                            UCHAR_UNIFIED_IDEOGRAPH },
            { _u("Uppercase"),                        UCHAR_UPPERCASE },
            { _u("Upper"),                            UCHAR_UPPERCASE },
            { _u("Variation_Selector"),               UCHAR_VARIATION_SELECTOR },
            { _u("VS"),                               UCHAR_VARIATION_SELECTOR },
            { _u("White_Space"),                      UCHAR_WHITE_SPACE },
            { _u("WSpace"),                           UCHAR_WHITE_SPACE },
            { _u("space"),                            UCHAR_WHITE_SPACE },
            { _u("XID_Continue"),                     UCHAR_XID_CONTINUE },
            { _u("XIDC"),                             UCHAR_XID_CONTINUE },
            { _u("XID_Start"),                        UCHAR_XID_START },
            { _u("XIDS"),                             UCHAR_XID_START },
        };

        // General_Category spellings ICU does not report through
        // u_getPropertyValueName enumeration: the eight grouped categories
        // (stored as U_GC_*_MASK values, marked isGroup) and two legacy
        // single-category aliases (stored as UCharCategory values).
        struct GcAliasName { const char16* name; int32 value; bool isGroup; };
        static const GcAliasName gcAliases[] =
        {
            { _u("C"),                static_cast<int32>(U_GC_C_MASK),  true },
            { _u("Other"),            static_cast<int32>(U_GC_C_MASK),  true },
            { _u("L"),                static_cast<int32>(U_GC_L_MASK),  true },
            { _u("Letter"),           static_cast<int32>(U_GC_L_MASK),  true },
            { _u("LC"),               static_cast<int32>(U_GC_LC_MASK), true },
            { _u("Cased_Letter"),     static_cast<int32>(U_GC_LC_MASK), true },
            { _u("M"),                static_cast<int32>(U_GC_M_MASK),  true },
            { _u("Mark"),             static_cast<int32>(U_GC_M_MASK),  true },
            { _u("Combining_Mark"),   static_cast<int32>(U_GC_M_MASK),  true },
            { _u("N"),                static_cast<int32>(U_GC_N_MASK),  true },
            { _u("Number"),           static_cast<int32>(U_GC_N_MASK),  true },
            { _u("P"),                static_cast<int32>(U_GC_P_MASK),  true },
            { _u("Punctuation"),      static_cast<int32>(U_GC_P_MASK),  true },
            { _u("punct"),            static_cast<int32>(U_GC_P_MASK),  true },
            { _u("S"),                static_cast<int32>(U_GC_S_MASK),  true },
            { _u("Symbol"),           static_cast<int32>(U_GC_S_MASK),  true },
            { _u("Z"),                static_cast<int32>(U_GC_Z_MASK),  true },
            { _u("Separator"),        static_cast<int32>(U_GC_Z_MASK),  true },
            { _u("cntrl"),            U_CONTROL_CHAR, false },
            { _u("digit"),            U_DECIMAL_DIGIT_NUMBER, false },
        };

        // Script aliases beyond each script's long/short name (the only two
        // extras in the ECMAScript tables).
        struct ScriptAliasName { const char16* name; int32 script; };
        static const ScriptAliasName scriptAliases[] =
        {
            { _u("Qaai"), USCRIPT_INHERITED },
            { _u("Qaac"), USCRIPT_COPTIC },
        };

        static bool EqualsAscii(const char16* name, CharCount nameLen, const char16* spelling)
        {
            for (CharCount i = 0; ; i++)
            {
                if (i == nameLen)
                    return spelling[i] == 0;
                if (spelling[i] == 0 || name[i] != spelling[i])
                    return false;
            }
        }

        // Exact, case-sensitive comparison of a UTF-16 candidate against an
        // ICU property name (always ASCII).
        static bool EqualsIcuName(const char16* value, CharCount valueLen, const char* spelling)
        {
            if (spelling == nullptr)
                return false;
            for (CharCount i = 0; ; i++)
            {
                char16 c = i < valueLen ? value[i] : 0;
                char s = spelling[i];
                if (c >= 0x80 || s < 0 || static_cast<char>(c) != s)
                    return false;
                if (c == 0)
                    return true; // both ended
            }
        }

        static bool ResolveGcValue(const char16* value, CharCount valueLen, Resolved* out)
        {
            for (int32 v = u_getIntPropertyMinValue(UCHAR_GENERAL_CATEGORY);
                 v <= u_getIntPropertyMaxValue(UCHAR_GENERAL_CATEGORY);
                 v++)
            {
                if (EqualsIcuName(value, valueLen, u_getPropertyValueName(UCHAR_GENERAL_CATEGORY, v, U_LONG_PROPERTY_NAME)) ||
                    EqualsIcuName(value, valueLen, u_getPropertyValueName(UCHAR_GENERAL_CATEGORY, v, U_SHORT_PROPERTY_NAME)))
                {
                    out->kind = GeneralCategory;
                    out->value = v;
                    out->isGroupValue = false;
                    return true;
                }
            }
            for (const GcAliasName& a : gcAliases)
            {
                if (EqualsAscii(value, valueLen, a.name))
                {
                    out->kind = GeneralCategory;
                    out->value = a.value;
                    out->isGroupValue = a.isGroup;
                    return true;
                }
            }
            return false;
        }

        static bool ResolveScriptValue(const char16* value, CharCount valueLen, Resolved* out)
        {
            for (int32 v = u_getIntPropertyMinValue(UCHAR_SCRIPT);
                 v <= u_getIntPropertyMaxValue(UCHAR_SCRIPT);
                 v++)
            {
                if (EqualsIcuName(value, valueLen, u_getPropertyValueName(UCHAR_SCRIPT, v, U_LONG_PROPERTY_NAME)) ||
                    EqualsIcuName(value, valueLen, u_getPropertyValueName(UCHAR_SCRIPT, v, U_SHORT_PROPERTY_NAME)))
                {
                    out->value = v;
                    out->isGroupValue = false;
                    return true;
                }
            }
            for (const ScriptAliasName& a : scriptAliases)
            {
                if (EqualsAscii(value, valueLen, a.name))
                {
                    out->value = a.script;
                    out->isGroupValue = false;
                    return true;
                }
            }
            return false;
        }

        bool Resolve(const char16* name, CharCount nameLen, const char16* value, CharCount valueLen, bool hasValue, Resolved* out)
        {
            if (nameLen == 0)
                return false;

            if (hasValue)
            {
                if (valueLen == 0)
                    return false;
                // Only the three non-binary properties accept Name=Value
                if (EqualsAscii(name, nameLen, _u("General_Category")) || EqualsAscii(name, nameLen, _u("gc")))
                    return ResolveGcValue(value, valueLen, out);
                if (EqualsAscii(name, nameLen, _u("Script")) || EqualsAscii(name, nameLen, _u("sc")))
                {
                    out->kind = Script;
                    return ResolveScriptValue(value, valueLen, out);
                }
                if (EqualsAscii(name, nameLen, _u("Script_Extensions")) || EqualsAscii(name, nameLen, _u("scx")))
                {
                    out->kind = ScriptExtensions;
                    return ResolveScriptValue(value, valueLen, out);
                }
                return false;
            }

            // Lone form: binary property name, then General_Category value.
            // (Script values are not valid in the lone form in ES2024.)
            for (const BinaryName& b : binaryNames)
            {
                if (EqualsAscii(name, nameLen, b.name))
                {
                    out->kind = BinaryProperty;
                    out->value = b.prop;
                    out->isGroupValue = false;
                    return true;
                }
            }
            if (EqualsAscii(name, nameLen, _u("Any")))
            {
                out->kind = Any;
                out->value = 0;
                out->isGroupValue = false;
                return true;
            }
            if (EqualsAscii(name, nameLen, _u("ASCII")))
            {
                out->kind = ASCII;
                out->value = 0;
                out->isGroupValue = false;
                return true;
            }
            if (EqualsAscii(name, nameLen, _u("Assigned")))
            {
                out->kind = Assigned;
                out->value = 0;
                out->isGroupValue = false;
                return true;
            }
            return ResolveGcValue(name, nameLen, out);
        }

        // Fills `result` with the USet's ranges. Property application never
        // produces string elements (that is v-flag "property of strings"
        // territory, out of scope), so any string item is skipped.
        static void CopyUSetRanges(USet* set, ArenaAllocator* allocator, CharSet<codepoint_t>& result)
        {
            UErrorCode ec = U_ZERO_ERROR;
            int32 itemCount = uset_getItemCount(set);
            for (int32 i = 0; i < itemCount; i++)
            {
                UChar32 start, end;
                UChar str[8];
                int32 strLen = uset_getItem(set, i, &start, &end, str, 8, &ec);
                if (U_FAILURE(ec))
                    break;
                if (strLen != 0)
                    continue;
                if (start > end)
                    continue;
                result.SetRange(allocator, static_cast<codepoint_t>(start), static_cast<codepoint_t>(end));
            }
        }

        void BuildCodePointSet(const Resolved& resolved, ArenaAllocator* allocator, CharSet<codepoint_t>& result)
        {
            switch (resolved.kind)
            {
                case Any:
                    result.SetRange(allocator, 0, 0x10FFFF);
                    return;
                case ASCII:
                    result.SetRange(allocator, 0, 0x7F);
                    return;
                case Invalid:
                    return;
                default:
                    break;
            }

            UErrorCode ec = U_ZERO_ERROR;
            USet* set = uset_open(1, 0); // empty
            switch (resolved.kind)
            {
                case BinaryProperty:
                    uset_applyIntPropertyValue(set, static_cast<UProperty>(resolved.value), 1, &ec);
                    break;
                case GeneralCategory:
                    uset_applyIntPropertyValue(set,
                        resolved.isGroupValue ? UCHAR_GENERAL_CATEGORY_MASK : UCHAR_GENERAL_CATEGORY,
                        resolved.value, &ec);
                    break;
                case Script:
                    uset_applyIntPropertyValue(set, UCHAR_SCRIPT, resolved.value, &ec);
                    break;
                case ScriptExtensions:
                {
                    // ICU exposes Script_Extensions only through property
                    // alias application; feed it the canonical long name of
                    // the resolved script code.
                    const char* longName = u_getPropertyValueName(UCHAR_SCRIPT, resolved.value, U_LONG_PROPERTY_NAME);
                    if (longName == nullptr)
                    {
                        ec = U_ILLEGAL_ARGUMENT_ERROR;
                        break;
                    }
                    UChar nameBuffer[32];
                    int32_t n = 0;
                    for (; longName[n] != 0 && n < 31; n++)
                        nameBuffer[n] = static_cast<UChar>(static_cast<unsigned char>(longName[n]));
                    nameBuffer[n] = 0;
                    uset_applyPropertyAlias(set, _u("Script_Extensions"), -1, nameBuffer, n, &ec);
                    break;
                }
                case Assigned:
                    uset_applyPropertyAlias(set, _u("Assigned"), -1, _u(""), 0, &ec);
                    break;
                default:
                    ec = U_ILLEGAL_ARGUMENT_ERROR;
                    break;
            }
            if (U_SUCCESS(ec))
                CopyUSetRanges(set, allocator, result);
            uset_close(set);
        }
    }
}
