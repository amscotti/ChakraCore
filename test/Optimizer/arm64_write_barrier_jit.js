//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Exercises all 6 ARM64 JIT write-barrier store paths under ConcurrentCollect:
//   1. InlineArrayPush (arr.push(youngStr/youngObj) + Promise.then + join)
//   2. StElemI_A (dynamic index in-bounds & segment-growing arr[arr.length] = youngObj)
//   3. StElemC (constant small offset arr[0] and large offset arr[600] > 4095)
//   4. StSlot / StSlotChkUndecl (captured var & let closure stores)
//   5. ConcatStringMulti (multi-operand young string concatenation)
//   6. StFld inline/aux/global slots, non-reg SymOpnd/AddrOpnd SinkDst,
//      arguments stack-slot store (-MachArgsSlotOffset), and non-triggerable sanity checks.

function assert(cond, msg) {
    if (!cond) {
        throw new Error("Assertion failed: " + msg);
    }
}

function assertEqual(actual, expected, msg) {
    if (actual !== expected) {
        throw new Error("Assertion failed (" + msg + "): expected " + String(expected) + ", got " + String(actual));
    }
}

function maybeGc() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

// Allocate enough short-lived objects/strings to push the Recycler into
// concurrent collection cycles without pinning the garbage.
function churnGarbage(seed, count) {
    var acc = 0;
    for (var i = 0; i < count; i++) {
        var tmp = { k: seed + i, s: "churn-" + seed + "-" + i };
        acc += tmp.k;
    }
    return acc;
}

// 1. InlineArrayPush: high-volume arr.push(youngStr) + Promise.then + join
function testInlineArrayPushConcurrent() {
    var buckets = [];
    for (var b = 0; b < 240; b++) {
        buckets.push(["init-" + b]);
    }

    function pushYoung(arr, tag, j) {
        var s = "push-" + tag + "-" + j;
        arr.push(s);
        return arr.length;
    }

    // Warm up pushYoung into FullJIT before concurrent Promise rounds.
    var warmupArr = [];
    for (var w = 0; w < 120; w++) {
        pushYoung(warmupArr, "w", w);
    }
    assertEqual(warmupArr.length, 120, "warmup push length");

    var promises = [];
    for (var i = 0; i < buckets.length; i++) {
        (function (idx) {
            var arr = buckets[idx];
            var p = Promise.resolve().then(function () {
                for (var j = 0; j < 12; j++) {
                    pushYoung(arr, idx, j);
                }
                churnGarbage(idx, 180);
                return Promise.resolve().then(function () {
                    pushYoung(arr, idx, 99);
                    if ((idx % 40) === 0) {
                        maybeGc();
                    }
                    var joined = arr.join("|");
                    assert(joined.indexOf("init-" + idx + "|push-" + idx + "-0") === 0, "InlineArrayPush join prefix " + idx);
                    assert(joined.indexOf("|push-" + idx + "-99") > 0, "InlineArrayPush join suffix " + idx);
                });
            });
            promises.push(p);
        })(i);
    }
    return Promise.all(promises);
}

// 2. StElemI_A: dynamic index (in-bounds & segment-growing arr[arr.length] = youngObj)
function testStElemIConcurrent() {
    function storeInBounds(arr, idx, val) {
        arr[idx] = val;
    }

    function storeGrow(arr, val) {
        arr[arr.length] = val;
    }

    var warm = [null, null, null, null];
    for (var w = 0; w < 120; w++) {
        storeInBounds(warm, w & 3, { id: w, tag: "wb-" + w });
        var g = [];
        storeGrow(g, { id: w, tag: "wg-" + w });
        storeGrow(g, { id: w + 1, tag: "wg2-" + w });
    }

    var liveInBounds = [];
    var liveGrow = [];
    for (var i = 0; i < 160; i++) {
        liveInBounds.push([null, null, null, null]);
        liveGrow.push([]);
    }

    var tasks = [];
    for (var r = 0; r < liveInBounds.length; r++) {
        (function (idx) {
            tasks.push(Promise.resolve().then(function () {
                var a = liveInBounds[idx];
                var b = liveGrow[idx];
                for (var k = 0; k < 4; k++) {
                    storeInBounds(a, k, { id: idx * 10 + k, label: "ib-" + idx + "-" + k });
                    storeGrow(b, { id: idx * 10 + k, label: "gr-" + idx + "-" + k });
                }
                churnGarbage(idx + 500, 140);
                return Promise.resolve().then(function () {
                    for (var k = 0; k < 4; k++) {
                        assertEqual(a[k].label, "ib-" + idx + "-" + k, "StElemI in-bounds " + idx + ":" + k);
                        assertEqual(b[k].label, "gr-" + idx + "-" + k, "StElemI grow " + idx + ":" + k);
                    }
                });
            }));
        })(r);
    }
    return Promise.all(tasks);
}

// 3. StElemC: small constant offset (arr[0]) and large offset (arr[600], 24 + 4800 = 4824 > 4095)
function testStElemCConcurrent() {
    function storeSmallConst(arr, v0, v1) {
        arr[0] = v0;
        arr[1] = v1;
    }

    function storeLargeConst(arr, v600, v601) {
        arr[600] = v600;
        arr[601] = v601;
    }

    function makeBigArray() {
        var a = new Array(604);
        for (var i = 0; i < 604; i++) {
            a[i] = null;
        }
        return a;
    }

    var wSmall = [null, null, null];
    var wBig = makeBigArray();
    for (var w = 0; w < 120; w++) {
        storeSmallConst(wSmall, { k: w, s: "sc0-" + w }, { k: w, s: "sc1-" + w });
        storeLargeConst(wBig, { k: w, s: "lc0-" + w }, { k: w, s: "lc1-" + w });
    }

    var smallArrs = [];
    var bigArrs = [];
    for (var i = 0; i < 120; i++) {
        smallArrs.push([null, null, null]);
        bigArrs.push(makeBigArray());
    }

    var tasks = [];
    for (var r = 0; r < smallArrs.length; r++) {
        (function (idx) {
            tasks.push(Promise.resolve().then(function () {
                storeSmallConst(smallArrs[idx], { s: "s0-" + idx }, { s: "s1-" + idx });
                storeLargeConst(bigArrs[idx], { s: "l0-" + idx }, { s: "l1-" + idx });
                churnGarbage(idx + 1000, 140);
                return Promise.resolve().then(function () {
                    assertEqual(smallArrs[idx][0].s, "s0-" + idx, "StElemC small[0] " + idx);
                    assertEqual(smallArrs[idx][1].s, "s1-" + idx, "StElemC small[1] " + idx);
                    assertEqual(bigArrs[idx][600].s, "l0-" + idx, "StElemC large[600] " + idx);
                    assertEqual(bigArrs[idx][601].s, "l1-" + idx, "StElemC large[601] " + idx);
                });
            }));
        })(r);
    }
    return Promise.all(tasks);
}

// 4. StSlot / StSlotChkUndecl: captured var & let closure stores under concurrent GC
function testStSlotConcurrent() {
    function makeSlotPair() {
        var capVar = { tag: "init-var" };
        let capLet = { tag: "init-let" };
        return {
            update: function (n) {
                for (var i = 0; i < 20; i++) {
                    capVar = { tag: "v-" + n + "-" + i };
                    capLet = { tag: "l-" + n + "-" + i };
                }
            },
            read: function () {
                return capVar.tag + "|" + capLet.tag;
            }
        };
    }

    var warmPair = makeSlotPair();
    for (var w = 0; w < 80; w++) {
        warmPair.update(w);
    }
    assertEqual(warmPair.read(), "v-79-19|l-79-19", "StSlot warmup");

    // Verify StSlotChkUndecl triggers write barriers on captured let and still throws ReferenceError before initialization.
    function testTdzInLoop(trigger, val) {
        if (trigger) {
            x = val;
        }
        let x = { tdz: val };
        return function () { return x; };
    }
    for (var t = 0; t < 100; t++) {
        assertEqual(testTdzInLoop(false, "ok-" + t)().tdz, "ok-" + t, "StSlotChkUndecl normal path");
    }
    var tdzThrew = false;
    try {
        testTdzInLoop(true, "bad");
    } catch (e) {
        tdzThrew = (e instanceof ReferenceError);
    }
    assert(tdzThrew, "StSlotChkUndecl TDZ throws ReferenceError");

    var pairs = [];
    for (var i = 0; i < 120; i++) {
        pairs.push(makeSlotPair());
    }
    var tasks = [];
    for (var r = 0; r < pairs.length; r++) {
        (function (idx) {
            tasks.push(Promise.resolve().then(function () {
                pairs[idx].update(idx);
                churnGarbage(idx + 2000, 140);
                return Promise.resolve().then(function () {
                    assertEqual(pairs[idx].read(), "v-" + idx + "-19|l-" + idx + "-19", "StSlot readback " + idx);
                });
            }));
        })(r);
    }
    return Promise.all(tasks);
}

// 5. ConcatStringMulti: multi-operand string concatenation storing young string parts
function testConcatStringMultiConcurrent() {
    var DELIM = "--concat-delim--";
    function buildConcat(a, b, c, d, n) {
        var p1 = "Alpha" + a;
        var p2 = "Bravo" + b;
        var p3 = "Charlie" + c;
        var p4 = "Delta" + d;
        var p5 = "Echo" + n;
        return p1 + "--concat-delim--" + p2 + "--concat-delim--" + p3 + "--concat-delim--" + p4 + "--concat-delim--" + p5;
    }

    for (var w = 0; w < 150; w++) {
        assertEqual(buildConcat(w, w + 1, w + 2, w + 3, w + 4),
            "Alpha" + w + DELIM + "Bravo" + (w + 1) + DELIM + "Charlie" + (w + 2) + DELIM + "Delta" + (w + 3) + DELIM + "Echo" + (w + 4),
            "ConcatStringMulti warmup");
    }

    var results = [];
    for (var i = 0; i < 160; i++) {
        results.push(null);
    }
    var tasks = [];
    for (var r = 0; r < results.length; r++) {
        (function (idx) {
            tasks.push(Promise.resolve().then(function () {
                for (var k = 0; k < 15; k++) {
                    results[idx] = buildConcat(idx, k, idx + k, k * 2, idx + 100);
                }
                churnGarbage(idx + 3000, 140);
                return Promise.resolve().then(function () {
                    var expected = "Alpha" + idx + DELIM + "Bravo14" + DELIM + "Charlie" + (idx + 14) + DELIM + "Delta28" + DELIM + "Echo" + (idx + 100);
                    assertEqual("" + results[idx], expected, "ConcatStringMulti readback " + idx);
                });
            }));
        })(r);
    }
    return Promise.all(tasks);
}

// 6. StFld (inline + aux + global/MemRef), SinkDst (SymOpnd/AddrOpnd), arguments (-MachArgsSlotOffset),
//    and non-triggerable tagged-int / float / TypedArray stores.
var globalBarrierBox = { tag: "init" };
function storeGlobalMemRef(v) {
    globalBarrierBox = v;
}

function testStFldSinkArgsAndNonTriggerable() {
    function makeInlineObj() {
        return { a: null, b: null };
    }

    function makeAuxObj() {
        return {
            p0: 0, p1: 1, p2: 2, p3: 3, p4: 4, p5: 5, p6: 6, p7: 7,
            p8: 8, p9: 9, p10: 10, p11: 11, p12: 12, p13: 13, p14: 14, p15: 15,
            p16: 16, p17: 17, p18: null, p19: null
        };
    }

    function storeInlineAndAux(inObj, auxObj, v1, v2) {
        inObj.a = v1;
        inObj.b = v2;
        auxObj.p18 = v1;
        auxObj.p19 = v2;
    }

    // High register pressure / spilled SymOpnd and constant AddrOpnd stores (SinkDst paths)
    function storeWithSpillAndConst(arr, o, n) {
        var s0 = { s: "sp0-" + n };
        var s1 = { s: "sp1-" + n };
        var s2 = { s: "sp2-" + n };
        var s3 = { s: "sp3-" + n };
        var s4 = { s: "sp4-" + n };
        var s5 = { s: "sp5-" + n };
        var s6 = { s: "sp6-" + n };
        var s7 = { s: "sp7-" + n };
        var s8 = { s: "sp8-" + n };
        var s9 = { s: "sp9-" + n };
        var s10 = { s: "sp10-" + n };
        var s11 = { s: "sp11-" + n };
        var s12 = { s: "sp12-" + n };
        var s13 = { s: "sp13-" + n };
        var s14 = { s: "sp14-" + n };
        var s15 = { s: "sp15-" + n };
        var s16 = { s: "sp16-" + n };
        var s17 = { s: "sp17-" + n };
        var s18 = { s: "sp18-" + n };
        var s19 = { s: "sp19-" + n };
        // Force spill across helper call
        churnGarbage(n, 2);
        arr[0] = s0; arr[1] = s1; arr[2] = s2; arr[3] = s3; arr[4] = s4;
        arr[5] = s5; arr[6] = s6; arr[7] = s7; arr[8] = s8; arr[9] = s9;
        arr[10] = s10; arr[11] = s11; arr[12] = s12; arr[13] = s13; arr[14] = s14;
        arr[15] = s15; arr[16] = s16; arr[17] = s17; arr[18] = s18; arr[19] = s19;
        arr[20] = "const-str-addr";
        o.a = "const-fld-str";
        o.b = s19;
    }

    // Heap arguments object stored to negative stack offset (-MachArgsSlotOffset)
    function useArgumentsSlot(x, y) {
        var args = arguments;
        churnGarbage(x.v + y.v, 5);
        return args[0].tag + ":" + args[1].tag + ":" + args.length;
    }

    // Non-triggerable store sanity checks (should NOT trigger write barriers)
    function storeNonTriggerable(intArr, dblArr, obj, u32, f64, i) {
        intArr[0] = i | 0;
        intArr[1] = (i + 1) | 0;
        dblArr[0] = i * 1.5;
        dblArr[1] = i * 2.25;
        obj.flag = true;
        obj.nil = null;
        obj.undef = undefined;
        u32[i & 15] = (i * 3) >>> 0;
        f64[i & 15] = i + 0.5;
    }

    var wIn = makeInlineObj();
    var wAux = makeAuxObj();
    var wSpillArr = new Array(22);
    var wIntArr = [1, 2, 3];
    var wDblArr = [1.5, 2.5, 3.5];
    var wPrimObj = { flag: false, nil: 1, undef: 1 };
    var wU32 = new Uint32Array(16);
    var wF64 = new Float64Array(16);

    for (var w = 0; w < 120; w++) {
        storeGlobalMemRef({ tag: "g-" + w });
        storeInlineAndAux(wIn, wAux, { tag: "in-" + w }, { tag: "aux-" + w });
        storeWithSpillAndConst(wSpillArr, wIn, w);
        assertEqual(useArgumentsSlot({ v: w, tag: "a" + w }, { v: w + 1, tag: "b" + w }),
            "a" + w + ":b" + w + ":2", "arguments slot warmup");
        storeNonTriggerable(wIntArr, wDblArr, wPrimObj, wU32, wF64, w);
    }
    assertEqual(globalBarrierBox.tag, "g-119", "global MemRef warmup");
    assertEqual(wSpillArr[19].s, "sp19-119", "spill SinkDst warmup");
    assertEqual(wSpillArr[20], "const-str-addr", "const AddrOpnd SinkDst warmup");
    assertEqual(wIn.a, "const-fld-str", "StFld const string warmup");
    assertEqual(wIn.b.s, "sp19-119", "StFld spilled sym warmup");
    assertEqual(wPrimObj.flag, true, "prim bool");
    assertEqual(wPrimObj.nil, null, "prim null");
    assertEqual(wPrimObj.undef, undefined, "prim undef");

    var inObjs = [];
    var auxObjs = [];
    var spillArrs = [];
    for (var i = 0; i < 100; i++) {
        inObjs.push(makeInlineObj());
        auxObjs.push(makeAuxObj());
        spillArrs.push(new Array(22));
    }

    var tasks = [];
    for (var r = 0; r < inObjs.length; r++) {
        (function (idx) {
            tasks.push(Promise.resolve().then(function () {
                storeInlineAndAux(inObjs[idx], auxObjs[idx], { tag: "in-" + idx }, { tag: "aux-" + idx });
                storeWithSpillAndConst(spillArrs[idx], makeInlineObj(), idx);
                storeGlobalMemRef({ tag: "g-async-" + idx });
                var argRes = useArgumentsSlot({ v: idx, tag: "x" + idx }, { v: idx + 1, tag: "y" + idx });
                assertEqual(argRes, "x" + idx + ":y" + idx + ":2", "arguments slot async " + idx);
                storeNonTriggerable(wIntArr, wDblArr, wPrimObj, wU32, wF64, idx);
                churnGarbage(idx + 4000, 120);
                return Promise.resolve().then(function () {
                    assertEqual(inObjs[idx].a.tag, "in-" + idx, "StFld inline.a " + idx);
                    assertEqual(inObjs[idx].b.tag, "aux-" + idx, "StFld inline.b " + idx);
                    assertEqual(auxObjs[idx].p18.tag, "in-" + idx, "StFld aux.p18 " + idx);
                    assertEqual(auxObjs[idx].p19.tag, "aux-" + idx, "StFld aux.p19 " + idx);
                    assertEqual(spillArrs[idx][0].s, "sp0-" + idx, "spill[0] " + idx);
                    assertEqual(spillArrs[idx][19].s, "sp19-" + idx, "spill[19] " + idx);
                    assertEqual(spillArrs[idx][20], "const-str-addr", "const[20] " + idx);
                });
            }));
        })(r);
    }
    return Promise.all(tasks);
}

Promise.all([
    testInlineArrayPushConcurrent(),
    testStElemIConcurrent(),
    testStElemCConcurrent(),
    testStSlotConcurrent(),
    testConcatStringMultiConcurrent(),
    testStFldSinkArgsAndNonTriggerable()
]).then(
    function () {
        if (typeof WScript !== "undefined" && WScript.Echo) {
            WScript.Echo("PASS");
        } else {
            print("PASS");
        }
    },
    function (err) {
        if (typeof WScript !== "undefined" && WScript.Echo) {
            WScript.Echo("FAIL: " + (err && err.stack ? err.stack : err));
        } else {
            print("FAIL: " + (err && err.stack ? err.stack : err));
        }
    }
);
