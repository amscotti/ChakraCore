//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// RV-01 regression: AsyncGenerator requestQueue (DList<AsyncGeneratorRequest*, Recycler>)
// and args.Values pending write-barrier blocks must stay traced across GC.

var failures = [];

function check(cond, msg) {
    if (!cond) {
        failures.push(msg);
        print("async-generator-gc-stress FAIL: " + msg);
    }
}

function maybeGC() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

function churnHeap() {
    var arr = [];
    for (var i = 0; i < 200; i++) {
        arr.push({ idx: i, nested: [i, i + 1, "s" + i] });
    }
    return arr.length;
}

var allPromises = [];

function runQueuedStress(round) {
    var resolvers = [];

    function deferredStep(tag) {
        return new Promise(function (resolve) {
            resolvers.push({ resolve: resolve, tag: tag });
        });
    }

    async function* agf(a0, a1, a2, a3, a4, a5, a6, a7) {
        churnHeap();
        maybeGC();
        var v1 = await deferredStep("await1-" + round);
        churnHeap();
        maybeGC();
        var r1 = yield { step: 1, v1: v1, sum: a0 + a1 + a2 + a3 };
        churnHeap();
        maybeGC();
        var v2 = await deferredStep("await2-" + round);
        churnHeap();
        maybeGC();
        var r2 = yield { step: 2, r1: r1, v2: v2, tail: a4 + a5 + a6 + a7 };
        return "done-" + round + "-" + r2;
    }

    var g = agf(1, 2, 3, 4, 10, 20, 30, 40);
    // Queue multiple requests simultaneously so requestQueue holds multiple nodes across GC.
    var p1 = g.next("unused");
    var p2 = g.next("arg-for-yield1");
    var p3 = g.next("arg-for-yield2");
    var p4 = g.next("after-done");

    maybeGC();
    churnHeap();
    maybeGC();

    // Drive the pending awaits while requests remain queued.
    var tick = Promise.resolve().then(function () {
        churnHeap();
        maybeGC();
        if (resolvers.length > 0) {
            resolvers.shift().resolve("res1-" + round);
        }
        return Promise.resolve().then(function () {
            churnHeap();
            maybeGC();
            return Promise.resolve().then(function () {
                churnHeap();
                maybeGC();
                if (resolvers.length > 0) {
                    resolvers.shift().resolve("res2-" + round);
                }
            });
        });
    });

    allPromises.push(Promise.all([p1, p2, p3, p4, tick]).then(function (res) {
        var r1 = res[0];
        var r2 = res[1];
        var r3 = res[2];
        var r4 = res[3];
        check(r1.done === false && r1.value.step === 1 && r1.value.v1 === "res1-" + round && r1.value.sum === 10,
            "round " + round + " step 1 mismatch");
        check(r2.done === false && r2.value.step === 2 && r2.value.r1 === "arg-for-yield1" && r2.value.v2 === "res2-" + round && r2.value.tail === 100,
            "round " + round + " step 2 mismatch");
        check(r3.done === true && r3.value === "done-" + round + "-arg-for-yield2",
            "round " + round + " return mismatch");
        check(r4.done === true && r4.value === undefined,
            "round " + round + " completed mismatch");
    }));
}

function runReturnThrowQueueStress(round) {
    var trigger;
    async function* agf(secret) {
        try {
            await new Promise(function (resolve) { trigger = resolve; });
            yield "first-" + secret;
            yield "second-" + secret;
        } catch (e) {
            return "caught-" + secret + "-" + e;
        }
    }

    var g = agf("k" + round);
    var p1 = g.next();
    var p2 = g.throw("err-" + round);
    var p3 = g.return("ret-" + round);

    maybeGC();
    churnHeap();
    maybeGC();

    allPromises.push(Promise.resolve().then(function () {
        maybeGC();
        trigger();
        return Promise.all([p1, p2, p3]).then(function (res) {
            check(res[0].done === false && res[0].value === "first-k" + round, "rt round " + round + " p1");
            check(res[1].done === true && res[1].value === "caught-k" + round + "-err-" + round, "rt round " + round + " p2");
            check(res[2].done === true && res[2].value === "ret-" + round, "rt round " + round + " p3");
        });
    }));
}

for (var i = 0; i < 15; i++) {
    runQueuedStress(i);
    runReturnThrowQueueStress(i);
}

Promise.all(allPromises).then(function () {
    if (failures.length > 0) {
        WScript.Quit(1);
    }
    print("pass");
}, function (err) {
    print("async-generator-gc-stress UNHANDLED: " + err);
    WScript.Quit(1);
});
