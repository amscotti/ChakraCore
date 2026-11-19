//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function fail(message) {
    print("iterator_concat: FAIL: " + message);
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
    assertSame(actual.length, expected.length, (message || "array") + " length");
    for (var i = 0; i < expected.length; i++) {
        assertSame(actual[i], expected[i], (message || "array") + "[" + i + "]");
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

// 1. Function metadata, descriptor, non-constructible, helper prototype
(function testMetadataAndPrototype() {
    assertSame(typeof Iterator.concat, "function", "typeof Iterator.concat");
    assertSame(Object.getPrototypeOf(Iterator.concat), Function.prototype, "Iterator.concat [[Prototype]]");

    var desc = Object.getOwnPropertyDescriptor(Iterator, "concat");
    assertTrue(desc !== undefined, "Iterator.concat descriptor exists");
    assertSame(desc.writable, true, "Iterator.concat writable");
    assertSame(desc.enumerable, false, "Iterator.concat enumerable");
    assertSame(desc.configurable, true, "Iterator.concat configurable");

    var lenDesc = Object.getOwnPropertyDescriptor(Iterator.concat, "length");
    assertSame(lenDesc.value, 0, "Iterator.concat.length");
    assertSame(lenDesc.writable, false, "length writable");
    assertSame(lenDesc.enumerable, false, "length enumerable");
    assertSame(lenDesc.configurable, true, "length configurable");

    var nameDesc = Object.getOwnPropertyDescriptor(Iterator.concat, "name");
    assertSame(nameDesc.value, "concat", "Iterator.concat.name");
    assertSame(nameDesc.writable, false, "name writable");
    assertSame(nameDesc.enumerable, false, "name enumerable");
    assertSame(nameDesc.configurable, true, "name configurable");

    assertThrows(function () { new Iterator.concat(); }, TypeError, "new Iterator.concat");
    assertThrows(function () { Reflect.construct(Iterator.concat, []); }, TypeError, "Reflect.construct Iterator.concat");

    var emptyHelper = Iterator.concat();
    var mapHelper = [1].values().map(function (x) { return x; });
    assertSame(Object.getPrototypeOf(emptyHelper), Object.getPrototypeOf(mapHelper), "%IteratorHelperPrototype%");
    assertTrue(emptyHelper instanceof Iterator, "concat helper instanceof Iterator");
    assertSame(Object.prototype.toString.call(emptyHelper), "[object Iterator Helper]", "toStringTag");

    // `this` receiver is ignored.
    assertArrayEquals(Array.from(Iterator.concat.call(null, [1, 2])), [1, 2], "null receiver");
    assertArrayEquals(Array.from(Iterator.concat.call(123, [3])), [3], "primitive receiver");
})();

// 2. Eager validation, primitive rejection, wrapper objects, GetMethod rules, order
(function testArgumentValidation() {
    var nonObjects = [undefined, null, true, false, 0, 123, -1.5, NaN, "", "abc", Symbol()];
    if (typeof BigInt === "function") {
        nonObjects.push(BigInt(123));
    }
    for (var i = 0; i < nonObjects.length; i++) {
        (function (val) {
            assertThrows(function () { Iterator.concat(val); }, TypeError, "non-object arg " + String(val));
        })(nonObjects[i]);
    }

    // String wrapper objects work; primitive strings throw.
    assertArrayEquals(Array.from(Iterator.concat(new String("ab"), Object("cd"))), ["a", "b", "c", "d"], "String wrapper objects");

    // Other primitive wrapper objects with Symbol.iterator succeed.
    var origBoolIter = Object.getOwnPropertyDescriptor(Boolean.prototype, Symbol.iterator);
    var origNumIter = Object.getOwnPropertyDescriptor(Number.prototype, Symbol.iterator);
    var origSymIter = Object.getOwnPropertyDescriptor(Symbol.prototype, Symbol.iterator);
    var origBigIntIter = typeof BigInt === "function" ? Object.getOwnPropertyDescriptor(BigInt.prototype, Symbol.iterator) : undefined;
    try {
        var wrapperDesc = {
            value: function () {
                var self = this;
                var done = false;
                return {
                    next: function () {
                        if (done) {
                            return { value: undefined, done: true };
                        }
                        done = true;
                        return { value: self.valueOf(), done: false };
                    }
                };
            },
            writable: true,
            enumerable: false,
            configurable: true
        };
        Object.defineProperty(Boolean.prototype, Symbol.iterator, wrapperDesc);
        Object.defineProperty(Number.prototype, Symbol.iterator, wrapperDesc);
        Object.defineProperty(Symbol.prototype, Symbol.iterator, wrapperDesc);
        if (typeof BigInt === "function") {
            Object.defineProperty(BigInt.prototype, Symbol.iterator, wrapperDesc);
        }
        var sym = Symbol("s");
        var args = [Object(true), Object(42), Object(sym)];
        if (typeof BigInt === "function") {
            args.push(Object(BigInt(99)));
        }
        var res = Array.from(Iterator.concat.apply(undefined, args));
        assertSame(res[0], true, "Boolean wrapper");
        assertSame(res[1], 42, "Number wrapper");
        assertSame(res[2], sym, "Symbol wrapper");
        if (typeof BigInt === "function") {
            assertSame(res[3], BigInt(99), "BigInt wrapper");
        }
    } finally {
        if (origBoolIter) Object.defineProperty(Boolean.prototype, Symbol.iterator, origBoolIter);
        else delete Boolean.prototype[Symbol.iterator];
        if (origNumIter) Object.defineProperty(Number.prototype, Symbol.iterator, origNumIter);
        else delete Number.prototype[Symbol.iterator];
        if (origSymIter) Object.defineProperty(Symbol.prototype, Symbol.iterator, origSymIter);
        else delete Symbol.prototype[Symbol.iterator];
        if (typeof BigInt === "function") {
            if (origBigIntIter) Object.defineProperty(BigInt.prototype, Symbol.iterator, origBigIntIter);
            else delete BigInt.prototype[Symbol.iterator];
        }
    }

    // Missing, null, undefined, or non-callable @@iterator throws TypeError eagerly.
    var badMethods = [undefined, null, true, 123, "fn", Symbol(), {}];
    if (typeof BigInt === "function") {
        badMethods.push(BigInt(1));
    }
    assertThrows(function () { Iterator.concat({}); }, TypeError, "no @@iterator");
    assertThrows(function () { Iterator.concat({ next: function () { return { done: true }; } }); }, TypeError, "iterator without @@iterator");
    for (var j = 0; j < badMethods.length; j++) {
        (function (m) {
            var obj = {};
            obj[Symbol.iterator] = m;
            assertThrows(function () { Iterator.concat(obj); }, TypeError, "non-callable @@iterator " + String(m));
        })(badMethods[j]);
    }

    // Proxy and revoked Proxy handling.
    var revocable = Proxy.revocable([1], {});
    revocable.revoke();
    assertThrows(function () { Iterator.concat(revocable.proxy); }, TypeError, "revoked proxy arg");

    var callableProxyIter = {
        [Symbol.iterator]: new Proxy(function () {
            assertSame(arguments.length, 0, "callable proxy openMethod receives 0 args");
            return [7, 8][Symbol.iterator]();
        }, {})
    };
    assertArrayEquals(Array.from(Iterator.concat(callableProxyIter)), [7, 8], "callable proxy @@iterator");

    // Left-to-right validation stops on first invalid argument.
    var getCount = 0;
    var iterable1 = {
        get [Symbol.iterator]() {
            getCount++;
            return function () { throw new Error("must not open"); };
        }
    };
    var iterable2 = {
        get [Symbol.iterator]() {
            throw new Error("must not inspect iterable2");
        }
    };
    assertThrows(function () { Iterator.concat(iterable1, null, iterable2); }, TypeError, "stops at null");
    assertSame(getCount, 1, "iterable1 getter ran once");

    // Custom error thrown from getter propagates.
    var Sentinel = function () {};
    assertThrows(function () {
        Iterator.concat({
            get [Symbol.iterator]() { throw new Sentinel(); }
        });
    }, Sentinel, "custom getter error");

    // @@iterator getter read only once eagerly; later mutation does not affect opening.
    var gets = 0;
    var opens = 0;
    var captured = {
        get [Symbol.iterator]() {
            gets++;
            return function () {
                opens++;
                assertSame(this, captured, "openMethod this === iterable");
                assertSame(arguments.length, 0, "openMethod 0 args");
                return [10, 20][Symbol.iterator]();
            };
        }
    };
    var iter = Iterator.concat(captured);
    assertSame(gets, 1, "getter called once eagerly");
    assertSame(opens, 0, "openMethod not called eagerly");
    Object.defineProperty(captured, Symbol.iterator, {
        get: function () { throw new Error("must not re-read Symbol.iterator"); }
    });
    assertArrayEquals(Array.from(iter), [10, 20], "uses captured openMethod");
    assertSame(gets, 1, "getter still called once");
    assertSame(opens, 1, "openMethod called once");
})();

// 3. Lazy opening, 0 args, many args, fresh result, 0-arg next, value after done
(function testIterationSemantics() {
    var empty = Iterator.concat();
    var r0 = empty.next();
    assertSame(r0.done, true, "0-arg done");
    assertSame(r0.value, undefined, "0-arg value");
    var r1 = empty.next();
    assertSame(r1.done, true, "0-arg repeat done");
    assertSame(r1.value, undefined, "0-arg repeat value");

    // Lazy opening in order across empty and non-empty iterables.
    var opened = [];
    function trackedIterable(tag, values) {
        var obj = {};
        obj[Symbol.iterator] = function () {
            opened.push(tag);
            return values[Symbol.iterator]();
        };
        return obj;
    }
    var seq = Iterator.concat(
        trackedIterable("a", []),
        trackedIterable("b", [1]),
        trackedIterable("c", []),
        trackedIterable("d", [2, 3]),
        trackedIterable("e", [])
    );
    assertArrayEquals(opened, [], "nothing opened before first next");
    assertSame(seq.next().value, 1, "first value skips empty a");
    assertArrayEquals(opened, ["a", "b"], "opened a and b");
    assertSame(seq.next().value, 2, "second value opens c and d");
    assertArrayEquals(opened, ["a", "b", "c", "d"], "opened up to d");
    assertSame(seq.next().value, 3, "third value from d");
    assertArrayEquals(opened, ["a", "b", "c", "d"], "e still unopened");
    var end = seq.next();
    assertSame(end.done, true, "exhausted");
    assertSame(end.value, undefined, "exhausted value");
    assertArrayEquals(opened, ["a", "b", "c", "d", "e"], "e opened when draining");

    // Fresh iterator result object and 0-arg next.
    var oldResult = { done: false, value: 123 };
    var nextGets = 0;
    var customIter = {
        get next() {
            nextGets++;
            return function () {
                assertSame(arguments.length, 0, "inner next called with 0 args");
                return oldResult;
            };
        }
    };
    var fresh = Iterator.concat({
        [Symbol.iterator]: function () { return customIter; }
    });
    var res = fresh.next(1, 2, 3);
    assertSame(nextGets, 1, "next cached once by GetIteratorDirect");
    assertSame(res.done, false, "fresh done");
    assertSame(res.value, 123, "fresh value");
    assertTrue(res !== oldResult, "fresh result object");
    assertSame(fresh.next("x").value, 123, "second next uses cached next");
    assertSame(nextGets, 1, "next still cached once");

    // Value getter not accessed when done is true.
    var valueGets = 0;
    var doneIter = {
        [Symbol.iterator]: function () {
            return {
                next: function () {
                    return {
                        get value() { valueGets++; throw new Error("must not access value"); },
                        done: true
                    };
                },
                return: function () { throw new Error("must not close done iterator"); }
            };
        }
    };
    assertArrayEquals(Array.from(Iterator.concat(doneIter, doneIter)), [], "done iterables");
    assertSame(valueGets, 0, "value getter untouched when done");
})();

// 4. Error handling and non-closing semantics on step errors
(function testErrorPaths() {
    // openMethod returns non-object.
    var nonObjects = [undefined, null, true, 123, "abc", Symbol()];
    if (typeof BigInt === "function") {
        nonObjects.push(BigInt(7));
    }
    for (var i = 0; i < nonObjects.length; i++) {
        (function (retVal) {
            var neverCalled = false;
            var it = Iterator.concat(
                { [Symbol.iterator]: function () { return retVal; } },
                { [Symbol.iterator]: function () { neverCalled = true; return [][Symbol.iterator](); } }
            );
            assertThrows(function () { it.next(); }, TypeError, "non-object iterator " + String(retVal));
            assertSame(neverCalled, false, "second iterable not opened after open failure");
            assertSame(it.next().done, true, "completed after open failure");
        })(nonObjects[i]);
    }

    // Error getting iter.next must not call iter.return().
    var returnCalls = 0;
    var throwingNextGet = Iterator.concat({
        [Symbol.iterator]: function () {
            return {
                get next() { throw new RangeError("next getter boom"); },
                return: function () { returnCalls++; return {}; }
            };
        }
    });
    assertThrows(function () { throwingNextGet.next(); }, RangeError, "throwing next getter");
    assertSame(returnCalls, 0, "return not called when GetIteratorDirect throws");
    assertSame(throwingNextGet.next().done, true, "completed after GetIteratorDirect error");

    // Non-callable next, throwing next(), non-object result, throwing done, throwing value:
    // none of them may call iter.return().
    function assertNoCloseOnStepError(iterObj, ErrCtor, label) {
        var closed = 0;
        var secondOpened = false;
        iterObj.return = function () { closed++; return {}; };
        var it = Iterator.concat(
            { [Symbol.iterator]: function () { return iterObj; } },
            { [Symbol.iterator]: function () { secondOpened = true; return [1][Symbol.iterator](); } }
        );
        assertThrows(function () { it.next(); }, ErrCtor, label);
        assertSame(closed, 0, label + ": return must not be called");
        assertSame(secondOpened, false, label + ": later iterable not opened");
        assertSame(it.next().done, true, label + ": completed after error");
        assertSame(it.return().done, true, label + ": return after error is no-op");
        assertSame(closed, 0, label + ": return still not called");
    }

    assertNoCloseOnStepError({ next: 42 }, TypeError, "non-callable next");
    assertNoCloseOnStepError({ next: function () { throw new URIError("next boom"); } }, URIError, "throwing next()");
    assertNoCloseOnStepError({ next: function () { return null; } }, TypeError, "null next result");
    assertNoCloseOnStepError({ next: function () { return { get done() { throw new SyntaxError("done boom"); }, value: 1 }; } }, SyntaxError, "throwing done");
    assertNoCloseOnStepError({ next: function () { return { done: false, get value() { throw new EvalError("value boom"); } }; } }, EvalError, "throwing value");

    // Earlier exhausted iterable must not be closed if later iterable errors.
    var firstReturnCalls = 0;
    var two = Iterator.concat(
        {
            [Symbol.iterator]: function () {
                var done = false;
                return {
                    next: function () {
                        if (done) return { done: true };
                        done = true;
                        return { value: 1, done: false };
                    },
                    return: function () { firstReturnCalls++; return {}; }
                };
            }
        },
        { [Symbol.iterator]: function () { throw new Error("second open boom"); } }
    );
    assertSame(two.next().value, 1, "first value before second fails");
    assertThrows(function () { two.next(); }, Error, "second open throws");
    assertSame(firstReturnCalls, 0, "exhausted first iterator not closed");
})();

// 5. return() forwarding, 0 args, before start, after exhaustion, return error handling
(function testReturnForwarding() {
    // Before initial start: never opens any iterable.
    var openedBeforeStart = false;
    var beforeStart = Iterator.concat({
        [Symbol.iterator]: function () {
            openedBeforeStart = true;
            return {
                next: function () { throw new Error("should not call next"); },
                return: function () { throw new Error("should not call return"); }
            };
        }
    });
    var rBefore = beforeStart.return();
    assertSame(rBefore.done, true, "return before start done");
    assertSame(rBefore.value, undefined, "return before start value");
    assertSame(openedBeforeStart, false, "suspended-start return does not open");
    assertSame(beforeStart.next().done, true, "next after suspended-start return is done");
    assertSame(beforeStart.return().done, true, "second return is done");
    assertSame(openedBeforeStart, false, "still never opened");

    // Forwarded when suspended-yield on active inner iterator, with 0 args, only once.
    var retCalls = 0;
    var secondOpened = false;
    var active = Iterator.concat(
        {
            [Symbol.iterator]: function () {
                return {
                    next: function () { return { value: 11, done: false }; },
                    return: function () {
                        retCalls++;
                        assertSame(arguments.length, 0, "forwarded return gets 0 args");
                        return { done: true };
                    }
                };
            }
        },
        {
            [Symbol.iterator]: function () {
                secondOpened = true;
                return [22][Symbol.iterator]();
            }
        }
    );
    assertSame(active.next().value, 11, "yielded 11");
    assertSame(retCalls, 0, "return not yet called");
    var rMid = active.return(1, 2, 3);
    assertSame(rMid.done, true, "return done");
    assertSame(rMid.value, undefined, "return value");
    assertSame(retCalls, 1, "inner return called once");
    assertSame(secondOpened, false, "second iterable never opened on return");
    assertSame(active.return().done, true, "idempotent return");
    assertSame(active.next().done, true, "next after return is done");
    assertSame(retCalls, 1, "inner return still called once");

    // Inner iterator with null/undefined return method.
    [undefined, null].forEach(function (retMethod) {
        var it = Iterator.concat({
            [Symbol.iterator]: function () {
                return {
                    next: function () { return { value: 1, done: false }; },
                    return: retMethod
                };
            }
        });
        assertSame(it.next().value, 1, "null/undefined return setup");
        assertSame(it.return().done, true, "null/undefined return succeeds");
    });

    // Inner return non-callable, returning non-object, or throwing.
    var badReturn = Iterator.concat({
        [Symbol.iterator]: function () {
            return {
                next: function () { return { value: 1, done: false }; },
                return: 42
            };
        }
    });
    badReturn.next();
    assertThrows(function () { badReturn.return(); }, TypeError, "non-callable inner return");
    assertSame(badReturn.next().done, true, "completed after bad return");

    var nonObjReturnCalls = 0;
    var nonObjReturn = Iterator.concat({
        [Symbol.iterator]: function () {
            return {
                next: function () { return { value: 1, done: false }; },
                return: function () { nonObjReturnCalls++; return 5; }
            };
        }
    });
    nonObjReturn.next();
    assertThrows(function () { nonObjReturn.return(); }, TypeError, "non-object inner return result");
    assertSame(nonObjReturnCalls, 1, "return not re-invoked by CloseAllSwallow");
    assertSame(nonObjReturn.return().done, true, "completed after non-object return");
    assertSame(nonObjReturnCalls, 1, "still called once");

    // After exhaustion, return() is not forwarded.
    var afterExhaustRet = 0;
    var exhausted = Iterator.concat({
        [Symbol.iterator]: function () {
            return {
                next: function () { return { done: true, value: undefined }; },
                return: function () { afterExhaustRet++; return {}; }
            };
        }
    });
    assertSame(exhausted.next().done, true, "exhausted");
    assertSame(exhausted.return().done, true, "return after exhaustion");
    assertSame(afterExhaustRet, 0, "return not called after exhaustion");
})();

// 6. Reentrancy checks (executing generator state)
(function testReentrancy() {
    // Reentrant next/return from inside openMethod.
    var helper1;
    var caughtOpenNext, caughtOpenReturn;
    helper1 = Iterator.concat({
        [Symbol.iterator]: function () {
            try { helper1.next(); } catch (e) { caughtOpenNext = e; }
            try { helper1.return(); } catch (e) { caughtOpenReturn = e; }
            return [5][Symbol.iterator]();
        }
    });
    assertSame(helper1.next().value, 5, "recovers if openMethod catches reentrant TypeError");
    assertTrue(caughtOpenNext instanceof TypeError, "reentrant next in openMethod threw TypeError");
    assertTrue(caughtOpenReturn instanceof TypeError, "reentrant return in openMethod threw TypeError");

    // Uncaught reentrant next inside inner.next().
    var helper2;
    var enters2 = 0;
    helper2 = Iterator.concat({
        [Symbol.iterator]: function () {
            return {
                next: function () {
                    enters2++;
                    helper2.next();
                    return { done: false };
                }
            };
        }
    });
    assertThrows(function () { helper2.next(); }, TypeError, "reentrant next throws");
    assertSame(enters2, 1, "entered once");
    assertSame(helper2.next().done, true, "completed after uncaught reentrant throw");

    // Reentrant next and return inside inner.return() during helper.return().
    var helper3;
    var caughtCloseNext, caughtCloseReturn;
    var closeCount3 = 0;
    helper3 = Iterator.concat({
        [Symbol.iterator]: function () {
            return {
                next: function () { return { value: 1, done: false }; },
                return: function () {
                    closeCount3++;
                    try { helper3.next(); } catch (e) { caughtCloseNext = e; }
                    try { helper3.return(); } catch (e) { caughtCloseReturn = e; }
                    return { done: true };
                }
            };
        }
    });
    helper3.next();
    assertSame(helper3.return().done, true, "outer return finishes");
    assertTrue(caughtCloseNext instanceof TypeError, "reentrant next in return threw TypeError");
    assertTrue(caughtCloseReturn instanceof TypeError, "reentrant return in return threw TypeError");
    assertSame(closeCount3, 1, "inner return called once");
})();

// 7. GC stress & large argument count (> HEAD_CHUNK_SIZE)
(function testGcAndManyArgs() {
    var getterStep = 0;
    function makeGcIterable(val) {
        return {
            get [Symbol.iterator]() {
                getterStep++;
                gcNow();
                return function () {
                    gcNow();
                    var emitted = false;
                    return {
                        next: function () {
                            gcNow();
                            if (emitted) return { done: true, value: undefined };
                            emitted = true;
                            return { value: val, done: false };
                        }
                    };
                };
            }
        };
    }

    var inputs = [];
    var expected = [];
    for (var i = 0; i < 80; i++) {
        inputs.push(makeGcIterable({ idx: i }));
        expected.push(i);
    }
    var gcIter = Iterator.concat.apply(undefined, inputs);
    assertSame(getterStep, 80, "all 80 getters evaluated");
    inputs = null;
    gcNow();

    var actual = [];
    for (var v of gcIter) {
        actual.push(v.idx);
        if (v.idx % 10 === 0) {
            gcNow();
        }
    }
    assertArrayEquals(actual, expected, "80-argument GC stress iteration");
    assertSame(gcIter.next().done, true, "done after 80 iterables");
})();

// 8. Chaining with Iterator.prototype helpers
(function testChaining() {
    var out = Iterator.concat([1, 2], new Set([3, 4]), [5, 6].values())
        .filter(function (x) { return x % 2 === 1; })
        .map(function (x, i) { return x * 10 + i; })
        .drop(1)
        .take(2)
        .toArray();
    assertArrayEquals(out, [31, 52], "chained helpers");

    var flat = Iterator.concat([1, 2], [3]).flatMap(function (x) {
        return Iterator.concat([x], [x * 10]);
    }).toArray();
    assertArrayEquals(flat, [1, 10, 2, 20, 3, 30], "nested Iterator.concat in flatMap");
})();

// 9. Cross-realm iterables and helpers
(function testCrossRealm() {
    if (typeof WScript !== "undefined" && typeof WScript.LoadScript === "function") {
        var other = WScript.LoadScript(
            "var foreignArr = [10, 20];\n" +
            "function* foreignGen() { yield 30; yield 40; }\n" +
            "var foreignHelper = Iterator.concat([100, 200], [300]);\n" +
            "var foreignProtoNext = Object.getPrototypeOf(foreignHelper).next;\n" +
            "var foreignProtoReturn = Object.getPrototypeOf(foreignHelper).return;\n",
            "samethread"
        );

        assertArrayEquals(
            Array.from(Iterator.concat(other.foreignArr, other.foreignGen())),
            [10, 20, 30, 40],
            "local Iterator.concat over foreign iterables"
        );

        assertArrayEquals(
            Array.from(other.Iterator.concat([1, 2], other.foreignArr)),
            [1, 2, 10, 20],
            "foreign Iterator.concat over local and foreign iterables"
        );

        // Alternate local and foreign %IteratorHelperPrototype%.next calls mid-iteration.
        var localHelper = Iterator.concat(other.foreignArr, [50, 60]);
        assertSame(other.foreignProtoNext.call(localHelper).value, 10, "foreign next on local helper (1)");
        assertSame(localHelper.next().value, 20, "local next on local helper (2)");
        assertSame(other.foreignProtoNext.call(localHelper).value, 50, "foreign next on local helper (3)");
        assertSame(localHelper.next().value, 60, "local next on local helper (4)");
        assertSame(other.foreignProtoNext.call(localHelper).done, true, "foreign next done");

        // Cross-realm return forwarding.
        var closeableForeign = other.eval("var closedForeign = 0; ({ [Symbol.iterator]() { return { next() { return { value: 1, done: false }; }, return() { closedForeign++; return { done: true }; } }; } })");
        var localOnForeign = Iterator.concat(closeableForeign);
        assertSame(localOnForeign.next().value, 1, "step foreign before cross-realm return");
        assertSame(other.foreignProtoReturn.call(localOnForeign).done, true, "cross-realm return");
        assertSame(other.closedForeign, 1, "cross-realm return invoked foreign return once");

        // Local helper methods on foreign concat helper.
        var localProtoNext = Object.getPrototypeOf(Iterator.concat()).next;
        assertSame(localProtoNext.call(other.foreignHelper).value, 100, "local next on foreign helper (1)");
        assertSame(other.foreignHelper.next().value, 200, "foreign next on foreign helper (2)");
        assertSame(localProtoNext.call(other.foreignHelper).value, 300, "local next on foreign helper (3)");
        assertSame(localProtoNext.call(other.foreignHelper).done, true, "local next on foreign helper done");
    }
})();

print("PASS");
