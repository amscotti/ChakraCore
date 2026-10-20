//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) 2021 ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Common definitions used outside parser so that we don't have to include the whole Parser.h.

#pragma once

namespace Js
{
    typedef int32  ByteCodeLabel;       // Size of this match the offset size in layouts
    typedef uint32 RegSlot;
    typedef uint8  RegSlot_OneByte;
    typedef int8   RegSlot_OneSByte;
    typedef int16  RegSlot_TwoSByte;
    typedef uint16 RegSlot_TwoByte;
}

enum ErrorTypeEnum
{
    kjstError,
    kjstEvalError,
    kjstRangeError,
    kjstReferenceError,
    kjstSyntaxError,
    kjstTypeError,
    kjstURIError,
    kjstAggregateError,
    kjstWebAssemblyCompileError,
    kjstWebAssemblyRuntimeError,
    kjstWebAssemblyLinkError,
    kjstCustomError,
};

class ParseNode;
class ParseNodeFnc;
typedef ParseNode *ParseNodePtr;

struct Ident;
struct PidRefStack;
typedef Ident *IdentPtr;

struct ModuleImportOrExportEntry
{
    IdentPtr     moduleRequest;
    IdentPtr     importName;
    IdentPtr     localName;
    IdentPtr     exportName;
    PidRefStack* pidRefStack;
    charcount_t  offset;
    struct ModuleAttributeList* attributes;
};

// A single `with { key: "value", ... }` entry. Both key and value are
// IdentPtrs: keys are IdentifierName or StringLiteral tokens, values are
// always StringLiteral tokens. The list is kept sorted by key so that
// module-request equality is order-insensitive.
struct ModuleAttributeEntry
{
    IdentPtr key;
    IdentPtr value;
    ModuleAttributeEntry* next;
};

struct ModuleAttributeList
{
    ModuleAttributeEntry* head;
};

// A static module request: a specifier plus its WithClause attributes
// (nullptr when no WithClause was present, which is equivalent to an
// empty attribute list per ES2025).
struct ModuleRequestEntry
{
    IdentPtr specifier;
    ModuleAttributeList* attributes;
};

typedef SList<ModuleImportOrExportEntry, ArenaAllocator> ModuleImportOrExportEntryList;
typedef SList<IdentPtr, ArenaAllocator> IdentPtrList;
typedef SList<ModuleRequestEntry, ArenaAllocator> ModuleRequestList;
typedef JsUtil::BaseHashSet<IdentPtr, ArenaAllocator, PowerOf2SizePolicy> IdentPtrSet;

//
// Below was moved from scrutil.h to share with chakradiag.
//
#define HR(sc) ((HRESULT)(sc))
