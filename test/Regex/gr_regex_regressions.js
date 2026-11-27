//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Focused regressions for GR-FE11: extended \u{...} escapes as character-class
// range endpoints in non-u (Annex B), u, and v modes.

function assert(cond, msg) {
    if (!cond) {
        throw new Error("Assertion failed: " + msg);
    }
}

function assertSyntaxError(pattern, flags, label) {
    var threw = false;
    try {
        new RegExp(pattern, flags);
    } catch (e) {
        if (e instanceof SyntaxError) {
            threw = true;
        } else {
            throw new Error("Expected SyntaxError (" + label + "), got: " + e);
        }
    }
    assert(threw, "Expected SyntaxError (" + label + "): /" + pattern + "/" + flags);

    // Also test literal form via new Function.
    threw = false;
    try {
        new Function("return /" + pattern + "/" + flags + ";");
    } catch (e) {
        if (e instanceof SyntaxError) {
            threw = true;
        } else {
            throw new Error("Expected literal SyntaxError (" + label + "), got: " + e);
        }
    }
    assert(threw, "Expected literal SyntaxError (" + label + "): /" + pattern + "/" + flags);
}

// 1. [a-\u{60}] without u: Annex B parses as [a-u{60}].
var r1 = new RegExp("[a-\\u{60}]");
var r1Lit = /[a-\u{60}]/;
for (var r of [r1, r1Lit]) {
    assert(r.test("a"), "[a-\\u{60}] matches a");
    assert(r.test("t"), "[a-\\u{60}] matches t");
    assert(r.test("u"), "[a-\\u{60}] matches u");
    assert(r.test("{"), "[a-\\u{60}] matches {");
    assert(r.test("6"), "[a-\\u{60}] matches 6");
    assert(r.test("0"), "[a-\\u{60}] matches 0");
    assert(r.test("}"), "[a-\\u{60}] matches }");
    assert(!r.test("v"), "[a-\\u{60}] does not match v");
    assert(!r.test("`"), "[a-\\u{60}] does not match backtick (U+0060)");
}
assertSyntaxError("[a-\\u{60}]", "u", "[a-\\u{60}]/u bad range");
assertSyntaxError("[a-\\u{60}]", "v", "[a-\\u{60}]/v bad range");

// 2. Non-']' lookahead after \u{...} range end: [a-\u{60}z].
var r2 = /[a-\u{60}z]/;
assert(r2.test("t") && r2.test("6") && r2.test("z") && !r2.test("y"), "[a-\\u{60}z] matches a..u, {, 6, 0, }, z");
assertSyntaxError("[a-\\u{60}z]", "u", "[a-\\u{60}z]/u bad range");

// 3. [z-\u{7a}]: invalid without u ('z' > 'u'), valid with u ('z'..'z').
assertSyntaxError("[z-\\u{7a}]", "", "[z-\\u{7a}] non-u bad range");
var r3 = /[z-\u{7a}]/u;
assert(r3.test("z") && !r3.test("y") && !r3.test("u"), "[z-\\u{7a}]/u matches only z");

// 4. [\u{7f}-~]: valid without u ('}'..'~' + u, {, 7, f), invalid with u (0x7f > 0x7e).
var r4 = /[\u{7f}-~]/;
assert(r4.test("u") && r4.test("{") && r4.test("7") && r4.test("f") && r4.test("}") && r4.test("~") && !r4.test("a"),
    "[\\u{7f}-~] non-u matches u,{,7,f,},~");
assertSyntaxError("[\\u{7f}-~]", "u", "[\\u{7f}-~]/u bad range");

// 5. Astral start [\u{10000}-~]: valid without u ('}'..'~'), invalid with u.
var r5 = /[\u{10000}-~]/;
assert(r5.test("}") && r5.test("~") && r5.test("u") && r5.test("1") && !r5.test("a"),
    "[\\u{10000}-~] non-u matches }..~ and u,{,1,0");
assertSyntaxError("[\\u{10000}-~]", "u", "[\\u{10000}-~]/u bad range");

// 6. Astral end [z-\u{10000}]: invalid without u ('z' > 'u'), valid with u.
assertSyntaxError("[z-\\u{10000}]", "", "[z-\\u{10000}] non-u bad range");
var r6 = /[z-\u{10000}]/u;
assert(r6.test("z") && r6.test("\u{10000}") && !r6.test("a"), "[z-\\u{10000}]/u matches z..U+10000");

// 7. Both endpoints extended [\u{60}-\u{50}]: invalid in both modes ('}' > 'u' and 0x60 > 0x50).
assertSyntaxError("[\\u{60}-\\u{50}]", "", "[\\u{60}-\\u{50}] non-u bad range");
assertSyntaxError("[\\u{60}-\\u{50}]", "u", "[\\u{60}-\\u{50}]/u bad range");

// 8. Chained dashes after \u{...}.
assertSyntaxError("[a-\\u{60}-z]", "", "[a-\\u{60}-z] non-u bad trailing range }-z");
assertSyntaxError("[a-\\u{60}-z]", "u", "[a-\\u{60}-z]/u bad leading range a-`");

var r8 = /[a-\u{60}-~]/;
assert(r8.test("t") && r8.test("}") && r8.test("~") && !r8.test("v"), "[a-\\u{60}-~] non-u valid");
assertSyntaxError("[a-\\u{60}-~]", "u", "[a-\\u{60}-~]/u bad range");

assertSyntaxError("[a-\\u{70}-z]", "", "[a-\\u{70}-z] non-u bad }-z");
var r9 = /[a-\u{70}-z]/u;
assert(r9.test("a") && r9.test("p") && r9.test("-") && r9.test("z") && !r9.test("q"),
    "[a-\\u{70}-z]/u matches a..p, -, z");

// 9. v-flag set difference with \u{...} stays working.
var rv1 = /[\u{61}--\u{60}]/v;
assert(rv1.test("a") && !rv1.test("`"), "[\\u{61}--\\u{60}]/v matches a");
var rv2 = /[\u{20}--a]/v;
assert(rv2.test(" ") && !rv2.test("a"), "[\\u{20}--a]/v matches space");

print("PASS");
