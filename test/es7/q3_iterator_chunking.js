//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function fail(message) {
    print("q3_iterator_chunking: FAIL: " + message);
    WScript.Quit(1);
}

function assertTrue(cond, message) {
    if (!cond) {
        fail(message || "expected true");
    }
}

function assertSame(actual, expected, message) {
    if (!Object.is(actual, expected)) {
        fail((message || "mismatch") + " (expected " + String(expected) + ", got " + String(actual) + ")");
    }
}

function assertArrayEquals(actual, expected, message) {
    assertTrue(Array.isArray(actual), (message || "array") + " isArray");
    assertSame(actual.length, expected.length, (message || "array") + " length");
    for (var i = 0; i < expected.length; i++) {
        assertSame(actual[i], expected[i], (message || "array") + "[" + i + "]");
    }
}

function assertNestedArrayEquals(actual, expected, message) {
    assertSame(actual.length, expected.length, (message || "nested") + " outer length");
    for (var i = 0; i < expected.length; i++) {
        assertArrayEquals(actual[i], expected[i], (message || "nested") + "[" + i + "]");
    }
}

function assertThrows(fn, ErrorCtor, message) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        if (ErrorCtor) {
            assertTrue(e instanceof ErrorCtor, (message || "throw") + " (expected " + ErrorCtor.name + ", got " + e + ")");
        }
        return e;
    }
    fail((message || "expected throw") + " (did not throw)");
}

function gcNow() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

function* range(n) {
    for (var i = 0; i < n; i++) {
        yield i;
    }
}

// 1. Metadata, descriptors, non-constructible, helper prototype
(function testMetadataAndPrototype() {
    var methods = [
        { name: "chunks", fn: Iterator.prototype.chunks },
        { name: "windows", fn: Iterator.prototype.windows },
    ];
    var mapHelperProto = Object.getPrototypeOf([1].values().map(function (x) { return x; }));

    for (var i = 0; i < methods.length; i++) {
        var m = methods[i];
        assertSame(typeof m.fn, "function", "typeof " + m.name);
        assertSame(Object.getPrototypeOf(m.fn), Function.prototype, m.name + " [[Prototype]]");

        var desc = Object.getOwnPropertyDescriptor(Iterator.prototype, m.name);
        assertTrue(desc !== undefined, m.name + " descriptor exists");
        assertSame(desc.writable, true, m.name + " writable");
        assertSame(desc.enumerable, false, m.name + " enumerable");
        assertSame(desc.configurable, true, m.name + " configurable");

        var lenDesc = Object.getOwnPropertyDescriptor(m.fn, "length");
        assertSame(lenDesc.value, 1, m.name + ".length");
        assertSame(lenDesc.writable, false, m.name + ".length writable");
        assertSame(lenDesc.enumerable, false, m.name + ".length enumerable");
        assertSame(lenDesc.configurable, true, m.name + ".length configurable");

        var nameDesc = Object.getOwnPropertyDescriptor(m.fn, "name");
        assertSame(nameDesc.value, m.name, m.name + ".name");
        assertSame(nameDesc.writable, false, m.name + ".name writable");
        assertSame(nameDesc.enumerable, false, m.name + ".name enumerable");
        assertSame(nameDesc.configurable, true, m.name + ".name configurable");

        assertThrows(function () { new m.fn(1); }, TypeError, "new " + m.name);
        assertThrows(function () { Reflect.construct(m.fn, [1]); }, TypeError, "Reflect.construct " + m.name);
    }

    var cHelper = range(0).chunks(1);
    var wHelper = range(0).windows(1);
    assertSame(Object.getPrototypeOf(cHelper), mapHelperProto, "chunks helper prototype");
    assertSame(Object.getPrototypeOf(wHelper), mapHelperProto, "windows helper prototype");
    assertTrue(cHelper instanceof Iterator, "chunks helper instanceof Iterator");
    assertTrue(wHelper instanceof Iterator, "windows helper instanceof Iterator");
})();

// 2. Receiver validation & argument validation / close order
(function testValidationAndOrder() {
    var nonObjects = [null, undefined, 0, -1, "abc", true, false, Symbol("s"), 1n];
    for (var i = 0; i < nonObjects.length; i++) {
        assertThrows(function () { Iterator.prototype.chunks.call(nonObjects[i], 0); }, TypeError, "chunks non-object this");
        assertThrows(function () { Iterator.prototype.windows.call(nonObjects[i], 0, "bad"); }, TypeError, "windows non-object this");
    }

    var badNumberTypes = [
        undefined, null, true, false, "1", "2", {}, [2], Symbol("s"), 1n, new Number(2),
        NaN, Infinity, -Infinity, 0.5, 1.5, -0.5, -1.5
    ];
    for (var j = 0; j < badNumberTypes.length; j++) {
        var closedC = 0;
        var iterC = {
            get next() { fail("chunks next must not be read on invalid chunkSize"); },
            return() { closedC++; return {}; }
        };
        assertThrows(function () { Iterator.prototype.chunks.call(iterC, badNumberTypes[j]); }, TypeError, "chunks invalid chunkSize type/integral " + j);
        assertSame(closedC, 1, "chunks closed on invalid chunkSize " + j);

        var closedW = 0;
        var iterW = {
            get next() { fail("windows next must not be read on invalid windowSize"); },
            return() { closedW++; return {}; }
        };
        assertThrows(function () { Iterator.prototype.windows.call(iterW, badNumberTypes[j], "bad"); }, TypeError, "windows invalid windowSize type/integral " + j);
        assertSame(closedW, 1, "windows closed on invalid windowSize " + j);
    }

    // No ToNumber / ToString coercion on size or undersized
    var coerced = 0;
    var coercionTrap = {
        valueOf() { coerced++; return 2; },
        toString() { coerced++; return "allow-partial"; }
    };
    var closedCoerce = 0;
    var iterCoerce = {
        get next() { fail("next must not be read"); },
        return() { closedCoerce++; return {}; }
    };
    assertThrows(function () { Iterator.prototype.chunks.call(iterCoerce, coercionTrap); }, TypeError, "chunks no coercion");
    assertSame(coerced, 0, "chunks coercion not invoked");
    assertSame(closedCoerce, 1, "chunks closed on object size");

    closedCoerce = 0;
    assertThrows(function () { Iterator.prototype.windows.call(iterCoerce, coercionTrap); }, TypeError, "windows no size coercion");
    assertSame(coerced, 0, "windows size coercion not invoked");
    assertSame(closedCoerce, 1, "windows closed on object size");

    closedCoerce = 0;
    assertThrows(function () { Iterator.prototype.windows.call(iterCoerce, 2, coercionTrap); }, TypeError, "windows no undersized coercion");
    assertSame(coerced, 0, "windows undersized coercion not invoked");
    assertSame(closedCoerce, 1, "windows closed on object undersized");

    // Out-of-range integral numbers -> RangeError + close
    var outOfRange = [0, -0, -1, -42, 2 ** 32, 2 ** 32 + 1, 2 ** 53];
    for (var k = 0; k < outOfRange.length; k++) {
        var closedR = 0;
        var iterR = {
            get next() { fail("next must not be read on out of range"); },
            return() { closedR++; return {}; }
        };
        assertThrows(function () { Iterator.prototype.chunks.call(iterR, outOfRange[k]); }, RangeError, "chunks out of range " + k);
        assertSame(closedR, 1, "chunks closed on RangeError " + k);

        closedR = 0;
        assertThrows(function () { Iterator.prototype.windows.call(iterR, outOfRange[k], "bad"); }, RangeError, "windows out of range before undersized " + k);
        assertSame(closedR, 1, "windows closed on RangeError " + k);
    }

    // Invalid undersized values -> TypeError + close
    var badUndersized = [null, "", "only-full ", "ALLOW-PARTIAL", "bad", 0, 1, true, false, {}, new String("allow-partial"), Symbol("u"), 1n];
    for (var u = 0; u < badUndersized.length; u++) {
        var closedU = 0;
        var iterU = {
            get next() { fail("next must not be read on invalid undersized"); },
            return() { closedU++; return {}; }
        };
        assertThrows(function () { Iterator.prototype.windows.call(iterU, 2, badUndersized[u]); }, TypeError, "windows bad undersized " + u);
        assertSame(closedU, 1, "windows closed on bad undersized " + u);
    }

    // Throwing return getter / return() during argument validation preserves original error
    var getReturnCount = 0;
    var throwingReturnGetter = {
        get next() { fail("next should not be read"); },
        get return() { getReturnCount++; throw new Error("mask me"); }
    };
    assertThrows(function () { Iterator.prototype.chunks.call(throwingReturnGetter, 0); }, RangeError, "chunks preserves RangeError over return getter throw");
    assertSame(getReturnCount, 1, "chunks return getter consulted");
    assertThrows(function () { Iterator.prototype.windows.call(throwingReturnGetter, 2, "bad"); }, TypeError, "windows preserves TypeError over return getter throw");
    assertSame(getReturnCount, 2, "windows return getter consulted for windows");

    // GetIteratorDirect reads next once, and throws from getter without closing
    var throwNextIter = {
        get next() { throw new Error("boom next"); },
        get return() { fail("return must not be called when get next throws"); }
    };
    assertThrows(function () { Iterator.prototype.chunks.call(throwNextIter, 2); }, Error, "chunks propagates get next error");
    assertThrows(function () { Iterator.prototype.windows.call(throwNextIter, 2); }, Error, "windows propagates get next error");

    var nextGets = 0;
    var plainIter = {
        get next() {
            nextGets++;
            var n = 0;
            return function () {
                return n < 3 ? { value: n++, done: false } : { value: undefined, done: true };
            };
        }
    };
    assertNestedArrayEquals(Array.from(Iterator.prototype.chunks.call(plainIter, 2)), [[0, 1], [2]], "plain iterator chunks");
    assertSame(nextGets, 1, "chunks reads next once");
    assertNestedArrayEquals(Array.from(Iterator.prototype.windows.call(plainIter, 2)), [[0, 1], [1, 2]], "plain iterator windows");
    assertSame(nextGets, 2, "windows reads next once");
})();

// 3. Chunks & Windows functional semantics, fresh array identity, large boundary sizes
(function testChunksAndWindowsSemantics() {
    // Empty
    assertNestedArrayEquals(Array.from(range(0).chunks(2)), [], "chunks empty");
    assertNestedArrayEquals(Array.from(range(0).windows(2)), [], "windows empty only-full");
    assertNestedArrayEquals(Array.from(range(0).windows(2, "allow-partial")), [], "windows empty allow-partial");

    // Evenly divisible and partial chunks
    assertNestedArrayEquals(Array.from(range(4).chunks(2)), [[0, 1], [2, 3]], "chunks evenly divisible");
    assertNestedArrayEquals(Array.from(range(5).chunks(2)), [[0, 1], [2, 3], [4]], "chunks partial tail");
    assertNestedArrayEquals(Array.from(range(4).chunks(1)), [[0], [1], [2], [3]], "chunks size 1");
    assertNestedArrayEquals(Array.from(range(3).chunks(100)), [[0, 1, 2]], "chunks size > length");

    // Windows only-full / default / undefined / allow-partial
    assertNestedArrayEquals(Array.from(range(5).windows(2)), [[0, 1], [1, 2], [2, 3], [3, 4]], "windows(2) default");
    assertNestedArrayEquals(Array.from(range(5).windows(2, undefined)), [[0, 1], [1, 2], [2, 3], [3, 4]], "windows(2, undefined)");
    assertNestedArrayEquals(Array.from(range(5).windows(2, "only-full")), [[0, 1], [1, 2], [2, 3], [3, 4]], "windows(2, only-full)");
    assertNestedArrayEquals(Array.from(range(5).windows(2, "allow-partial")), [[0, 1], [1, 2], [2, 3], [3, 4]], "windows(2, allow-partial) full stream yields no duplicate tail");
    assertNestedArrayEquals(Array.from(range(2).windows(2, "allow-partial")), [[0, 1]], "windows(2, allow-partial) exact window");
    assertNestedArrayEquals(Array.from(range(2).windows(3)), [], "windows(3) short stream only-full");
    assertNestedArrayEquals(Array.from(range(2).windows(3, "allow-partial")), [[0, 1]], "windows(3, allow-partial) short stream");
    assertNestedArrayEquals(Array.from(range(4).windows(1)), [[0], [1], [2], [3]], "windows(1)");
    assertNestedArrayEquals(Array.from(range(6).windows(3)), [[0, 1, 2], [1, 2, 3], [2, 3, 4], [3, 4, 5]], "windows(3)");

    // Dynamic ConcatString for undersized
    var ap = "allow-" + "partial";
    var of = "only-" + "full";
    assertNestedArrayEquals(Array.from(range(2).windows(5, ap)), [[0, 1]], "windows dynamic ConcatString allow-partial");
    assertNestedArrayEquals(Array.from(range(2).windows(5, of)), [], "windows dynamic ConcatString only-full");

    // Large buffer growth beyond initial 4-element capacity + sliding wrap-around
    var win25 = Array.from(range(60).windows(25));
    assertSame(win25.length, 36, "windows(25) over 60 elements count");
    assertArrayEquals(win25[0], Array.from(range(25)), "windows(25) first");
    assertSame(win25[35][0], 35, "windows(25) last[0]");
    assertSame(win25[35][24], 59, "windows(25) last[24]");

    var ch17 = Array.from(range(40).chunks(17));
    assertSame(ch17.length, 3, "chunks(17) over 40 elements");
    assertSame(ch17[0].length, 17, "chunks(17)[0]");
    assertSame(ch17[1].length, 17, "chunks(17)[1]");
    assertSame(ch17[2].length, 6, "chunks(17)[2]");
    assertSame(ch17[2][5], 39, "chunks(17)[2][5]");

    // Boundary 2^32 - 1 and 2^31 do not preallocate 4B slots
    assertNestedArrayEquals(Array.from(range(3).chunks(2 ** 32 - 1)), [[0, 1, 2]], "chunks(2^32 - 1) short stream");
    assertNestedArrayEquals(Array.from(range(3).windows(2 ** 32 - 1)), [], "windows(2^32 - 1) only-full short stream");
    assertNestedArrayEquals(Array.from(range(3).windows(2 ** 32 - 1, "allow-partial")), [[0, 1, 2]], "windows(2^32 - 1) allow-partial short stream");
    assertNestedArrayEquals(Array.from(range(2).chunks(2 ** 31)), [[0, 1]], "chunks(2^31)");

    // Fresh distinct arrays & mutation isolation
    var wIter = range(4).windows(2);
    var w0 = wIter.next().value;
    w0[0] = 999;
    w0.push(888);
    var w1 = wIter.next().value;
    assertArrayEquals(w1, [1, 2], "mutating previous window does not affect next window");
    assertTrue(w0 !== w1, "distinct window array instances");

    var cIter = range(4).chunks(2);
    var c0 = cIter.next().value;
    c0[0] = 777;
    var c1 = cIter.next().value;
    assertArrayEquals(c1, [2, 3], "mutating previous chunk does not affect next chunk");
    assertTrue(c0 !== c1, "distinct chunk array instances");
})();

// 4. Return forwarding, exhaustion after partial yield, parallel advance/close, errors, reentrancy
(function testLifecycleAndReentrancy() {
    // Return forwarded on suspended-start and suspended-yield, only once
    var retCount = 0;
    function makeOpen() {
        var i = 0;
        return {
            __proto__: Iterator.prototype,
            next() { return { value: i++, done: false }; },
            return() { retCount++; return {}; }
        };
    }
    var h1 = makeOpen().chunks(2);
    assertSame(h1.return().done, true, "chunks suspended-start return");
    assertSame(retCount, 1, "chunks suspended-start forwarded return");
    assertSame(h1.return().done, true, "chunks second return after close");
    assertSame(retCount, 1, "chunks second return not forwarded");

    retCount = 0;
    var h2 = makeOpen().windows(2);
    assertArrayEquals(h2.next().value, [0, 1], "windows first yield");
    assertSame(h2.return().done, true, "windows mid-stream return");
    assertSame(retCount, 1, "windows mid-stream forwarded return");
    assertSame(h2.next().done, true, "windows next after return is done");

    // Partial final yield exhausts underlying iterator -> return() must NOT call underlying.return()
    var tailReturnCalls = 0;
    var oneItemChunkIter = {
        __proto__: Iterator.prototype,
        n: 0,
        next() {
            if (this.n === 0) { this.n = 1; return { value: "only", done: false }; }
            return { value: undefined, done: true };
        },
        return() { tailReturnCalls++; return {}; }
    };
    var chPartial = oneItemChunkIter.chunks(3);
    assertArrayEquals(chPartial.next().value, ["only"], "chunks partial tail yield");
    assertSame(chPartial.return().done, true, "chunks return after partial tail");
    assertSame(tailReturnCalls, 0, "chunks does not call underlying.return() after partial tail already observed done:true");
    assertSame(chPartial.next().done, true, "chunks next after return is done");

    var oneItemWinIter = {
        __proto__: Iterator.prototype,
        n: 0,
        next() {
            if (this.n === 0) { this.n = 1; return { value: "w", done: false }; }
            return { value: undefined, done: true };
        },
        return() { tailReturnCalls++; return {}; }
    };
    var winPartial = oneItemWinIter.windows(3, "allow-partial");
    assertArrayEquals(winPartial.next().value, ["w"], "windows allow-partial tail yield");
    assertSame(winPartial.return().done, true, "windows return after allow-partial tail");
    assertSame(tailReturnCalls, 0, "windows does not call underlying.return() after allow-partial tail observed done:true");

    // Calling next() after partial tail yield must NOT re-call underlying.next()
    var nextCallsAfterDone = 0;
    var strictDoneIter = {
        __proto__: Iterator.prototype,
        step: 0,
        next() {
            if (this.step === 0) { this.step = 1; return { value: 42, done: false }; }
            if (this.step === 1) { this.step = 2; return { value: undefined, done: true }; }
            nextCallsAfterDone++;
            fail("underlying.next() must not be called after done:true");
        }
    };
    var chStrict = strictDoneIter.chunks(2);
    assertArrayEquals(chStrict.next().value, [42], "partial chunk");
    assertSame(chStrict.next().done, true, "done after partial chunk");
    assertSame(nextCallsAfterDone, 0, "no extra next() call");

    var winStrict = {
        __proto__: Iterator.prototype,
        step: 0,
        next() {
            if (this.step === 0) { this.step = 1; return { value: 42, done: false }; }
            if (this.step === 1) { this.step = 2; return { value: undefined, done: true }; }
            nextCallsAfterDone++;
            fail("underlying.next() must not be called after done:true (windows)");
        }
    }.windows(2, "allow-partial");
    assertArrayEquals(winStrict.next().value, [42], "partial window");
    assertSame(winStrict.next().done, true, "done after partial window");
    assertSame(nextCallsAfterDone, 0, "no extra next() call (windows)");

    // Reentrancy during next() throws TypeError; uncaught completes & marks underlying done
    var reenterHelper;
    var reenterCalls = 0;
    var reenterRetCalls = 0;
    var reenterUnderlying = {
        next() {
            reenterCalls++;
            if (reenterCalls === 1) {
                return { value: "a", done: false };
            }
            reenterHelper.next();
            return { value: "b", done: false };
        },
        return() {
            reenterRetCalls++;
            return {};
        }
    };
    reenterHelper = Iterator.prototype.chunks.call(reenterUnderlying, 2);
    assertThrows(function () { reenterHelper.next(); }, TypeError, "reentrant next throws TypeError");
    assertSame(reenterCalls, 2, "reentrant next did not run inner loop");
    assertSame(reenterRetCalls, 0, "throw in underlying.next does not close underlying");
    assertSame(reenterHelper.next().done, true, "helper completed after reentrant throw");

    // Caught reentrancy inside next() leaves helper consistent
    var caughtReenterHelper;
    var stepIdx = 0;
    var caughtUnderlying = {
        next() {
            stepIdx++;
            if (stepIdx === 2) {
                assertThrows(function () { caughtReenterHelper.next(); }, TypeError, "caught reentrant next");
                assertThrows(function () { caughtReenterHelper.return(); }, TypeError, "caught reentrant return");
            }
            return stepIdx <= 3 ? { value: stepIdx, done: false } : { value: undefined, done: true };
        }
    };
    caughtReenterHelper = Iterator.prototype.windows.call(caughtUnderlying, 2);
    assertArrayEquals(caughtReenterHelper.next().value, [1, 2], "window survives caught reentrancy");
    assertArrayEquals(caughtReenterHelper.next().value, [2, 3], "second window after caught reentrancy");
    assertSame(caughtReenterHelper.next().done, true, "completed after caught reentrancy");

    // Reentrancy during suspended-start return() vs suspended-yield return()
    var suspStartHelper;
    var suspStartReentryResults = [];
    suspStartHelper = Iterator.prototype.chunks.call({
        next() { return { value: 1, done: false }; },
        return() {
            suspStartReentryResults.push(suspStartHelper.next().done);
            suspStartReentryResults.push(suspStartHelper.return().done);
            return {};
        }
    }, 2);
    assertSame(suspStartHelper.return().done, true, "suspended-start return succeeds");
    assertArrayEquals(suspStartReentryResults, [true, true], "reentry during suspended-start return sees completed");

    var suspYieldHelper;
    var suspYieldThrew = 0;
    suspYieldHelper = Iterator.prototype.windows.call({
        next() { return { value: 1, done: false }; },
        return() {
            assertThrows(function () { suspYieldHelper.next(); }, TypeError, "reentrant next in started return");
            assertThrows(function () { suspYieldHelper.return(); }, TypeError, "reentrant return in started return");
            suspYieldThrew++;
            return {};
        }
    }, 1);
    assertArrayEquals(suspYieldHelper.next().value, [1], "yield first window");
    assertSame(suspYieldHelper.return().done, true, "started return succeeds");
    assertSame(suspYieldThrew, 1, "reentry checks executed during started return");
})();

// 5. GC safety across buffered elements and sliding/chunking transitions
(function testGCSafety() {
    var step = 0;
    var gcIter = {
        next() {
            step++;
            if (step <= 6) {
                var payload = { tag: "item-" + step, nested: [step, step * 10] };
                // Force garbage collection between elements while earlier objects are only held inside chunkState!
                gcNow();
                return { value: payload, done: false };
            }
            return { value: undefined, done: true };
        }
    };

    var ch = Iterator.prototype.chunks.call(gcIter, 4);
    var firstChunk = ch.next().value;
    assertSame(firstChunk.length, 4, "gc chunks first length");
    for (var i = 0; i < 4; i++) {
        assertSame(firstChunk[i].tag, "item-" + (i + 1), "gc chunk element " + i);
        assertSame(firstChunk[i].nested[1], (i + 1) * 10, "gc chunk nested " + i);
    }
    gcNow();
    var secondChunk = ch.next().value;
    assertSame(secondChunk.length, 2, "gc chunks partial length");
    assertSame(secondChunk[0].tag, "item-5", "gc chunk2[0]");
    assertSame(secondChunk[1].tag, "item-6", "gc chunk2[1]");
    assertSame(ch.next().done, true, "gc chunks done");

    // Windows GC across sliding window shifts
    var wStep = 0;
    var gcWinIter = {
        next() {
            wStep++;
            if (wStep <= 8) {
                var obj = { id: wStep, data: { v: wStep * 3 } };
                gcNow();
                return { value: obj, done: false };
            }
            return { value: undefined, done: true };
        }
    };
    var wh = Iterator.prototype.windows.call(gcWinIter, 3);
    var seenWindows = [];
    var r;
    while (!(r = wh.next()).done) {
        gcNow();
        seenWindows.push(r.value.map(function (o) { return o.id * 100 + o.data.v; }));
    }
    assertSame(seenWindows.length, 6, "gc windows count");
    assertArrayEquals(seenWindows[0], [103, 206, 309], "gc window 0");
    assertArrayEquals(seenWindows[5], [618, 721, 824], "gc window 5");
})();

// 6. Cross-realm interoperability (same-thread)
(function testCrossRealm() {
    if (typeof WScript !== "undefined" && typeof WScript.LoadScript === "function") {
        var other = WScript.LoadScript(
            "var foreignArr = [10, 20, 30, 40, 50];" +
            "var foreignGen = function* () { yield 'a'; yield 'b'; yield 'c'; };" +
            "var foreignHelperProto = Object.getPrototypeOf([1].values().chunks(1));",
            "samethread"
        );
        assertNestedArrayEquals(
            Array.from(Iterator.prototype.chunks.call(other.foreignArr.values(), 2)),
            [[10, 20], [30, 40], [50]],
            "local chunks on foreign array iterator"
        );
        assertNestedArrayEquals(
            Array.from(Iterator.prototype.windows.call(other.foreignGen(), 2)),
            [["a", "b"], ["b", "c"]],
            "local windows on foreign generator"
        );
        var localHelper = [1, 2, 3].values().chunks(2);
        assertArrayEquals(other.foreignHelperProto.next.call(localHelper).value, [1, 2], "foreign helper.next on local chunks helper");
        assertArrayEquals(other.foreignHelperProto.next.call(localHelper).value, [3], "foreign helper.next partial on local chunks helper");
        assertSame(other.foreignHelperProto.next.call(localHelper).done, true, "foreign helper.next done");
    }
})();

// 7. JIT warmup across chunks, windows, validation errors, early return, and GC
(function testJITWarmup() {
    function runBatch(n) {
        var sum = 0;
        var arr = [1, 2, 3, 4, 5];
        var cIt = arr.values().chunks(2);
        var cr;
        while (!(cr = cIt.next()).done) {
            for (var i = 0; i < cr.value.length; i++) {
                sum += cr.value[i];
            }
        }
        var wIt = arr.values().windows(3);
        var wr;
        while (!(wr = wIt.next()).done) {
            sum += wr.value[0] + wr.value[1] + wr.value[2];
        }
        var wpIt = [10, 20].values().windows(4, "allow-partial");
        var wp = wpIt.next().value;
        sum += wp[0] + wp[1];
        if (!wpIt.next().done) {
            fail("expected done after allow-partial tail");
        }
        if (n % 50 === 0) {
            var early = arr.values().windows(2);
            sum += early.next().value[0];
            early.return();
        }
        return sum;
    }

    var expectedWithReturn = runBatch(0);
    assertSame(expectedWithReturn, 73, "jit warmup batch 0");
    for (var i = 1; i <= 250; i++) {
        var res = runBatch(i);
        if (i % 50 === 0) {
            assertSame(res, 73, "jit warmup batch with early return " + i);
            assertThrows(function () { [1].values().chunks(0); }, RangeError, "jit warmup RangeError");
            assertThrows(function () { [1].values().windows(1, "bad"); }, TypeError, "jit warmup TypeError");
        } else {
            assertSame(res, 72, "jit warmup batch " + i);
        }
    }
})();

print("PASS");
