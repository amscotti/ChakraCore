//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Regression test: Math.round(NaN) under FullJIT int32 speculation.
// The ARM64 fast inline path must check FPSR.IOC (via the overflow label)
// even when the converted int is a non-negative zero, otherwise +NaN takes
// the negZero early-exit and wrongly returns 0 instead of bailing out.
function r(x) { return Math.round(x); }
var out = [];
for (var i = 0; i < 3000; i++) {
    out.push("" + r(i % 101));
}
var s = "" + r(NaN);
if (s !== "NaN") {
    throw new Error("FAIL: Math.round(NaN) stringified as " + s);
}
print("PASS");
