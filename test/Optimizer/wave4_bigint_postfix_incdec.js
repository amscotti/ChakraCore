//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assertEqual(actual, expected, msg) {
    if (!Object.is(actual, expected)) {
        throw new Error(msg + ": expected " + String(expected) + " got " + String(actual));
    }
}

function assertThrowsTypeError(fn, msg) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        if (e instanceof TypeError) {
            threw = true;
        } else {
            throw new Error(msg + ": unexpected error " + e);
        }
    }
    if (!threw) {
        throw new Error(msg + ": expected TypeError");
    }
}

function postAddLoop(n) {
    var s = 0, j = 0;
    for (var i = 0; i < n; i++) {
        s = (s + j++) & 0xffff;
    }
    return [s, j];
}

function preAddLoop(n) {
    var s = 0, j = 0;
    for (var i = 0; i < n; i++) {
        s = (s + ++j) & 0xffff;
    }
    return [s, j];
}

function postAssignLoop(n) {
    var s = 0, j = 0, t = 0;
    for (var i = 0; i < n; i++) {
        t = j++;
        s = (s + t) | 0;
    }
    return [s, t, j];
}

function postSubLoop(n) {
    var s = 0, j = n;
    for (var i = 0; i < n; i++) {
        s = (s + j--) & 0xffff;
    }
    return [s, j];
}

function postCmpLoop(n) {
    var s = 0, j = 0;
    for (var i = 0; i < n; i++) {
        if (j++ > 100) {
            s++;
        }
    }
    return [s, j];
}

function postArrayIdx(n) {
    var a = [1, 2, 3, 4, 5, 6, 7, 8];
    var ta = new Int32Array(16);
    for (var k = 0; k < 16; k++) {
        ta[k] = k + 10;
    }
    var s = 0, j = 0;
    for (var i = 0; i < n; i++) {
        s = (s + a[j++ & 7] + ta[j-- & 15]) | 0;
        j++;
    }
    return [s, j];
}

function postIncAny(x) {
    var old = x++;
    return [old, x];
}

function postDecAny(x) {
    var old = x--;
    return [old, x];
}

function preIncAny(x) {
    var val = ++x;
    return [val, x];
}

function preDecAny(x) {
    var val = --x;
    return [val, x];
}

function postUnusedAny(x) {
    x++;
    return x;
}

function postIncHoistedLoop(x, n) {
    var last = 0;
    for (var i = 0; i < n; i++) {
        var cur = x;
        last = cur++;
    }
    return last;
}

for (var w = 0; w < 500; w++) {
    var pa = postAddLoop(200);
    assertEqual(pa[0], 19900, "postAddLoop sum");
    assertEqual(pa[1], 200, "postAddLoop j");

    var pra = preAddLoop(200);
    assertEqual(pra[0], 20100, "preAddLoop sum");
    assertEqual(pra[1], 200, "preAddLoop j");

    var pas = postAssignLoop(200);
    assertEqual(pas[0], 19900, "postAssignLoop sum");
    assertEqual(pas[1], 199, "postAssignLoop t");
    assertEqual(pas[2], 200, "postAssignLoop j");

    var ps = postSubLoop(200);
    assertEqual(ps[0], 20100, "postSubLoop sum");
    assertEqual(ps[1], 0, "postSubLoop j");

    var pc = postCmpLoop(200);
    assertEqual(pc[0], 99, "postCmpLoop s");
    assertEqual(pc[1], 200, "postCmpLoop j");

    var arrRes = postArrayIdx(64);
    assertEqual(arrRes[0], 1408, "postArrayIdx sum");
    assertEqual(arrRes[1], 64, "postArrayIdx j");

    var pi = postIncAny(w);
    assertEqual(pi[0], w, "warmup postIncAny old");
    assertEqual(pi[1], w + 1, "warmup postIncAny new");

    var pd = postDecAny(w);
    assertEqual(pd[0], w, "warmup postDecAny old");
    assertEqual(pd[1], w - 1, "warmup postDecAny new");

    var pri = preIncAny(w);
    assertEqual(pri[0], w + 1, "warmup preIncAny val");
    assertEqual(pri[1], w + 1, "warmup preIncAny new");

    var prd = preDecAny(w);
    assertEqual(prd[0], w - 1, "warmup preDecAny val");
    assertEqual(prd[1], w - 1, "warmup preDecAny new");

    assertEqual(postUnusedAny(w), w + 1, "warmup postUnusedAny");
    assertEqual(postIncHoistedLoop(w, 5), w, "warmup postIncHoistedLoop");
}

for (var iter = 0; iter < 300; iter++) {
    // Int32 overflow and -0 preservation.
    var rMax = postIncAny(0x7fffffff);
    assertEqual(rMax[0], 0x7fffffff, "INT32_MAX++ old");
    assertEqual(rMax[1], 0x80000000, "INT32_MAX++ new");

    var rMin = postDecAny(-0x80000000);
    assertEqual(rMin[0], -0x80000000, "INT32_MIN-- old");
    assertEqual(rMin[1], -0x80000001, "INT32_MIN-- new");

    var rNegZero = postIncAny(-0);
    assertEqual(rNegZero[0], -0, "-0++ old preserves -0");
    assertEqual(rNegZero[1], 1, "-0++ new");

    var rNegZeroDec = postDecAny(-0);
    assertEqual(rNegZeroDec[0], -0, "-0-- old preserves -0");
    assertEqual(rNegZeroDec[1], -1, "-0-- new");

    // Non-numeric primitives and objects.
    var rStr = postIncAny("41");
    assertEqual(rStr[0], 41, "string++ old");
    assertEqual(rStr[1], 42, "string++ new");

    var rUndef = postIncAny(undefined);
    assertEqual(rUndef[0], NaN, "undefined++ old");
    assertEqual(rUndef[1], NaN, "undefined++ new");

    var rNull = postDecAny(null);
    assertEqual(rNull[0], 0, "null-- old");
    assertEqual(rNull[1], -1, "null-- new");

    var rBool = postIncAny(true);
    assertEqual(rBool[0], 1, "true++ old");
    assertEqual(rBool[1], 2, "true++ new");

    var sideNum = 0;
    var rObjNum = postIncAny({ valueOf: function () { sideNum++; return 7; } });
    assertEqual(rObjNum[0], 7, "obj(7)++ old");
    assertEqual(rObjNum[1], 8, "obj(7)++ new");
    assertEqual(sideNum, 1, "obj(7)++ valueOf once");

    // BigInt values and { valueOf() { return BigInt; } }.
    var rBig = postIncAny(1n);
    assertEqual(rBig[0], 1n, "1n++ old");
    assertEqual(rBig[1], 2n, "1n++ new");

    var rBigWide = postIncAny(0xffffffffffffffffn);
    assertEqual(rBigWide[0], 0xffffffffffffffffn, "wide BigInt++ old");
    assertEqual(rBigWide[1], 0x10000000000000000n, "wide BigInt++ new");

    var rBigDec = postDecAny(0n);
    assertEqual(rBigDec[0], 0n, "0n-- old");
    assertEqual(rBigDec[1], -1n, "0n-- new");

    var rPreBig = preIncAny(-1n);
    assertEqual(rPreBig[0], 0n, "++(-1n) val");
    assertEqual(rPreBig[1], 0n, "++(-1n) new");

    var rPreDecBig = preDecAny(1n);
    assertEqual(rPreDecBig[0], 0n, "--(1n) val");
    assertEqual(rPreDecBig[1], 0n, "--(1n) new");

    assertEqual(postUnusedAny(41n), 42n, "unused 41n++");
    assertEqual(postIncHoistedLoop(5n, 5), 5n, "postIncHoistedLoop 5n");

    var sideBig = 0;
    var rObjBig = postIncAny({ valueOf: function () { sideBig++; return 7n; } });
    assertEqual(rObjBig[0], 7n, "obj(7n)++ old");
    assertEqual(rObjBig[1], 8n, "obj(7n)++ new");
    assertEqual(sideBig, 1, "obj(7n)++ valueOf once");

    assertThrowsTypeError(function () { return postIncAny(Symbol("x")); }, "Symbol++");
    assertThrowsTypeError(function () { return postDecAny(Symbol("x")); }, "Symbol--");
}

// Also verify a standalone BigInt-only function compiled with BigInt profile.
function bigIntOnlyPost(b) {
    var old = b++;
    var next = --b;
    return [old, next, b];
}
for (var k = 0; k < 400; k++) {
    var biRes = bigIntOnlyPost(10n);
    assertEqual(biRes[0], 10n, "bigIntOnlyPost old");
    assertEqual(biRes[1], 10n, "bigIntOnlyPost --b");
    assertEqual(biRes[2], 10n, "bigIntOnlyPost final");
}

print("PASS");
