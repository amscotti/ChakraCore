//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Post-8.0 case-fold pairs (Unicode 9.0: Adlam, Osage, Medefaidrin).
// Each pair must match in both directions under /iu, in classes and in
// \p{...} closures. Without /u, astral pairs do not fold (per-code-unit
// Canonicalize, matching V8/JSC).

var pairs = [
    ["\u{1E900}", "\u{1E922}"],
    ["\u{104B0}", "\u{104D8}"],
    ["\u{16E40}", "\u{16E60}"]
];

for (var k = 0; k < pairs.length; k++) {
    var up = pairs[k][0], lo = pairs[k][1];
    print("up-vs-lo /iu: " + new RegExp(up, "iu").test(lo));
    print("lo-vs-up /iu: " + new RegExp(lo, "iu").test(up));
    print("up-vs-lo /i: " + new RegExp(up, "i").test(lo));
    print("class /iu: " + new RegExp("[" + up + "]", "iu").test(lo));
}

print("Lu+Ll Adlam /iu: " + /[\p{Lu}\p{Ll}]/iu.test("\u{1E922}"));
print("Osage doubled /iu: " + /^\u{104B0}+$/iu.test("\u{104D8}\u{104D8}"));
print("Medefaidrin doubled /iu: " + /^\u{16E40}+$/iu.test("\u{16E60}\u{16E60}"));
