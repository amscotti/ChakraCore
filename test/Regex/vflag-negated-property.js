//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assertEq(actual, expected, msg) {
    if (actual !== expected) {
        throw new Error(msg + ": expected " + JSON.stringify(expected) + ", got " + JSON.stringify(actual));
    }
}

// RV-07: Under /iv, negated Unicode properties (\P{...}) must close the
// property set before complementing (both atom and class paths), and
// PropertyCompiledCache must distinguish /iu from /iv.

// 1. Atom \P{Lu} under /iv vs /iu (also exercises cache isolation in both orders).
assertEq(/\P{Lu}/iu.test("A"), true, "atom /\\P{Lu}/iu matches A");
assertEq(/\P{Lu}/iv.test("A"), false, "atom /\\P{Lu}/iv rejects A");
assertEq(/\P{Lu}/iv.test("a"), false, "atom /\\P{Lu}/iv rejects a");
assertEq(/\P{Lu}/iv.test("1"), true, "atom /\\P{Lu}/iv matches 1");
assertEq(/\P{Lu}/iu.test("a"), true, "atom /\\P{Lu}/iu still matches a after /iv");

// Reverse cache lookup order: warm /iv first with a distinct property, then test /iu.
assertEq(/\P{Lt}/iv.test("\u01C5"), false, "atom /\\P{Lt}/iv rejects titlecase");
assertEq(/\P{Lt}/iv.test("\u01C4"), false, "atom /\\P{Lt}/iv rejects uppercase equiv");
assertEq(/\P{Lt}/iv.test("\u01C6"), false, "atom /\\P{Lt}/iv rejects lowercase equiv");
assertEq(/\P{Lt}/iu.test("\u01C4"), true, "atom /\\P{Lt}/iu matches uppercase equiv");

// 2. Character class [\P{Lu}] and [^\P{Lu}] under /iv.
assertEq(/[\P{Lu}]/iv.test("A"), false, "[\\P{Lu}]/iv rejects A");
assertEq(/[\P{Lu}]/iv.test("a"), false, "[\\P{Lu}]/iv rejects a");
assertEq(/[\P{Lu}]/iv.test("1"), true, "[\\P{Lu}]/iv matches 1");
assertEq(/[\P{Lu}]/iu.test("A"), true, "[\\P{Lu}]/iu matches A");

var ohm = /[^\P{Lu}]/iv.exec("\u2126");
assertEq(ohm !== null && ohm[0], "\u2126", "[^\\P{Lu}]/iv matches U+2126 OHM SIGN");
assertEq(/[^\P{Lu}]/iv.test("\u03C9"), true, "[^\\P{Lu}]/iv matches lowercase omega");
assertEq(/[^\P{Lu}]/iv.test("a"), true, "[^\\P{Lu}]/iv matches a");
assertEq(/[^\P{Lu}]/iv.test("1"), false, "[^\\P{Lu}]/iv rejects 1");
assertEq(/[^\P{Lu}]/iu.exec("\u2126"), null, "[^\\P{Lu}]/iu rejects U+2126");

// Nested negated class and set operations with \P{...} under /iv.
assertEq(/[[^\P{Lu}]]/iv.test("\u2126"), true, "nested [[^\\P{Lu}]]/iv matches U+2126");
assertEq(/[[^\P{Lu}]]/iv.test("1"), false, "nested [[^\\P{Lu}]]/iv rejects 1");
assertEq(/[\P{Lu}&&\p{ASCII}]/iv.test("A"), false, "[\\P{Lu}&&\\p{ASCII}]/iv rejects A");
assertEq(/[\P{Lu}&&\p{ASCII}]/iv.test("a"), false, "[\\P{Lu}&&\\p{ASCII}]/iv rejects a");
assertEq(/[\P{Lu}&&\p{ASCII}]/iv.test("1"), true, "[\\P{Lu}&&\\p{ASCII}]/iv matches 1");

// String.prototype.split with /[\P{Lu}]/iv.
assertEq(JSON.stringify("Ab1".split(/[\P{Lu}]/iv)), JSON.stringify(["Ab", ""]), "split Ab1 by /[\\P{Lu}]/iv");

// 3. Scoped i-modifiers inside /v and /iv.
assertEq(/(?i:\P{Lu})/v.test("A"), false, "scoped (?i:\\P{Lu})/v rejects A");
assertEq(/(?i:\P{Lu})/v.test("a"), false, "scoped (?i:\\P{Lu})/v rejects a");
assertEq(/(?-i:\P{Lu})/iv.test("a"), true, "scoped (?-i:\\P{Lu})/iv matches lowercase a");
assertEq(/(?-i:\P{Lu})/iv.test("A"), false, "scoped (?-i:\\P{Lu})/iv rejects A");
assertEq(/(?i:[\P{Lu}])/v.test("A"), false, "scoped (?i:[\\P{Lu}])/v rejects A");
assertEq(/(?-i:[\P{Lu}])/iv.test("a"), true, "scoped (?-i:[\\P{Lu}])/iv matches a");

print("pass");
