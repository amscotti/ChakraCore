//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Verify ARM64 overflow formals (>5 user formals, ParamSlot offsets >=64 bytes,
// exceeding callee-saved GPR count so spills hit local stack slots) across calls,
// arithmetic/int/overflow/type bailouts, and try/catch EH unwind.

function callee(x) {
    return (x | 0) + 1;
}

function mayThrow(flag) {
    if (flag) {
        throw new Error("boom");
    }
    return 1;
}

function bailNoTry(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16, mode) {
    var c = callee(p1);
    var sum = 0;
    if (mode === 0) {
        sum = (p6 + c) + p7 + p8 + p9 + p10 + p11 + p12 + p13 + p14 + p15 + p16;
    } else if (mode === 1) {
        sum = p6 + p7 + p8 + p9 + p10 + p11 + p12 + p13 + p14 + p15 + p16 + c;
    } else {
        sum = (p6 * 2) + (p7 * 3) + (p8 * 4) + (p9 * 5) + (p10 * 6) +
              (p11 * 7) + (p12 * 8) + (p13 * 9) + (p14 * 10) + (p15 * 11) + (p16 * 12) + c;
    }
    return sum;
}

function bailWithEH(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16, shouldThrow) {
    var c = callee(p1);
    try {
        mayThrow(shouldThrow);
        return p6 + p7 + p8 + p9 + p10 + p11 + p12 + p13 + p14 + p15 + p16 + c;
    } catch (e) {
        return p6 * 2 + p7 * 3 + p8 * 4 + p9 * 5 + p10 * 6 +
               p11 * 7 + p12 * 8 + p13 * 9 + p14 * 10 + p15 * 11 + p16 * 12 + c;
    }
}

for (var i = 0; i < 500; i++) {
    var r0 = bailNoTry(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0);
    if (r0 !== 123) throw new Error("r0=" + r0);
    var r1 = bailNoTry(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 1);
    if (r1 !== 123) throw new Error("r1=" + r1);
    var r2 = bailNoTry(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 2);
    if (r2 !== 959) throw new Error("r2=" + r2);

    var e0 = bailWithEH(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, false);
    if (e0 !== 123) throw new Error("e0=" + e0);
    var e1 = bailWithEH(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, true);
    if (e1 !== 959) throw new Error("e1=" + e1);
}

var ov = bailNoTry(1, 2, 3, 4, 5, 0x7fffffff, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0);
if (ov !== 0x7fffffff + 2 + 115) throw new Error("ov=" + ov);

var fl = bailNoTry(1, 2, 3, 4, 5, 6.5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16.25, 1);
if (fl !== 6.5 + 16.25 + 101) throw new Error("fl=" + fl);

var st = bailNoTry(1, 2, 3, 4, 5, 6, 7, "X", 9, 10, 11, 12, 13, "Y", 15, 16, 1);
if (st !== "13X910111213Y15162") throw new Error("st=" + st);

var ov2 = bailNoTry(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0x40000000, 2);
if (ov2 !== 765 + 0x40000000 * 12 + 2) throw new Error("ov2=" + ov2);

print("Passed");
