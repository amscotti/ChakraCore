//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assertEq(actual, expected, msg) {
    if (actual !== expected) {
        throw new Error(msg + ": expected " + String(expected) + ", got " + String(actual));
    }
}

function assertSyntaxError(fn, msg) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = (e instanceof SyntaxError);
    }
    if (!threw) {
        throw new Error("Expected SyntaxError: " + msg);
    }
}

// RV-14: Non-u RegExp \u{DecimalDigits} followed by a quantifier must throw
// SyntaxError (and never hit debug AssertMsg or misparse '+' as literal).
assertSyntaxError(function () { new RegExp("\\u{61}+"); }, "new RegExp('\\\\u{61}+')");
assertSyntaxError(function () { new RegExp("\\u{61}+", "i"); }, "new RegExp('\\\\u{61}+', 'i')");
assertSyntaxError(function () { new RegExp("a\\u{61}+b"); }, "new RegExp('a\\\\u{61}+b')");
assertSyntaxError(function () { new RegExp("\\u{61}*"); }, "new RegExp('\\\\u{61}*')");
assertSyntaxError(function () { new RegExp("\\u{61}+?"); }, "new RegExp('\\\\u{61}+?')");
assertSyntaxError(function () { new RegExp("\\u{61}{2}"); }, "new RegExp('\\\\u{61}{2}')");
assertSyntaxError(function () { new RegExp("\\u{0}{2,5}"); }, "new RegExp('\\\\u{0}{2,5}')");
assertSyntaxError(function () { new RegExp("\\u{61}??"); }, "new RegExp('\\\\u{61}??')");
assertSyntaxError(function () { new RegExp("\\u{10000}+"); }, "new RegExp('\\\\u{10000}+')");
assertSyntaxError(function () { eval("/\\u{61}+/"); }, "literal /\\u{61}+/");
assertSyntaxError(function () { eval("function f() { return /\\u{61}+/; }"); }, "deferred literal /\\u{61}+/");
assertSyntaxError(function () { eval("function g() { return /\\u{61}{2}/; }"); }, "deferred literal /\\u{61}{2}/");

// Valid non-u \u{DecimalDigits} with no extra quantifier (or single lazy '?'):
// \u is identity 'u' and {N} is the quantifier.
var re61 = new RegExp("^\\u{61}$");
assertEq(re61.test("u".repeat(61)), true, "\\u{61} repeats 'u' 61 times");
assertEq(re61.test("a"), false, "\\u{61} is not 'a'");

var re61Lazy = new RegExp("^\\u{61}?$");
assertEq(re61Lazy.test("u".repeat(61)), true, "\\u{61}? is lazy {61}? on 'u'");

var reGroup = new RegExp("^(\\u{3})+$");
assertEq(reGroup.test("uuuuuu"), true, "(\\u{3})+ repeats group of uuu");

// Valid u and v mode \u{...} with quantifiers still work.
assertEq(/^\u{61}+$/u.test("aaa"), true, "/\\u{61}+/u matches aaa");
assertEq(/^\u{61}+$/v.test("aaa"), true, "/\\u{61}+/v matches aaa");
assertEq(/^\u{61}??$/u.test(""), true, "/\\u{61}??/u matches empty");
assertEq(/^\u{10000}+$/u.test("\u{10000}\u{10000}"), true, "/\\u{10000}+/u matches astral");

// Preserved Annex B non-u patterns.
assertEq(/^[\u{61}]+$/.test("u{61}"), true, "[\\u{61}]+ matches chars of u{61}");
assertEq(/^\p{L}$/.test("p{L}"), true, "\\p{L} without u is literal p{L}");
assertEq(/^\p{L}+$/.test("p{L}}}"), true, "\\p{L}+ without u quantifies '}'");
assertEq(/^\k<n>$/.test("k<n>"), true, "\\k<n> without named group is literal");
assertEq(/^\u{1z}$/.test("u{1z}"), true, "\\u{1z} without u is literal");
assertEq(/^\u{a}+$/.test("u{a}}}"), true, "\\u{a}+ without u quantifies '}'");
assertSyntaxError(function () { new RegExp("(?<a>x)\\k<b>"); }, "(?<a>x)\\k<b> throws SyntaxError");

print("pass");
