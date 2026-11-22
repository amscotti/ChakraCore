//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function fail(message) {
    print("q3_iterator_zip: FAIL: " + message);
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

function ExpectedError(tag) {
    this.tag = tag;
}

// 1. Metadata, property descriptors, non-constructible, helper prototype
(function testMetadata() {
    var helperProto = Object.getPrototypeOf([1].values().map(function (x) { return x; }));
    var names = ["zip", "zipKeyed"];
    for (var i = 0; i < names.length; i++) {
        var name = names[i];
        var fn = Iterator[name];
        assertSame(typeof fn, "function", "typeof Iterator." + name);
        assertSame(Object.getPrototypeOf(fn), Function.prototype, name + " [[Prototype]]");

        var propDesc = Object.getOwnPropertyDescriptor(Iterator, name);
        assertTrue(propDesc !== undefined, name + " descriptor");
        assertSame(propDesc.writable, true, name + " writable");
        assertSame(propDesc.enumerable, false, name + " enumerable");
        assertSame(propDesc.configurable, true, name + " configurable");

        var lenDesc = Object.getOwnPropertyDescriptor(fn, "length");
        assertSame(lenDesc.value, 1, name + ".length");
        assertSame(lenDesc.writable, false, name + ".length writable");
        assertSame(lenDesc.enumerable, false, name + ".length enumerable");
        assertSame(lenDesc.configurable, true, name + ".length configurable");

        var nameDesc = Object.getOwnPropertyDescriptor(fn, "name");
        assertSame(nameDesc.value, name, name + ".name");
        assertSame(nameDesc.writable, false, name + ".name writable");
        assertSame(nameDesc.enumerable, false, name + ".name enumerable");
        assertSame(nameDesc.configurable, true, name + ".name configurable");

        assertThrows(function () { new fn([]); }, TypeError, "new Iterator." + name);
        assertThrows(function () { Reflect.construct(fn, [{}]); }, TypeError, "Reflect.construct " + name);

        var inst = fn(name === "zip" ? [] : {});
        assertTrue(inst instanceof Iterator, name + " result instanceof Iterator");
        assertSame(Object.getPrototypeOf(inst), helperProto, name + " helper [[Prototype]]");
    }
})();

// 2. Options validation, evaluation order, and primitive rejection
(function testOptionsAndArguments() {
    var nonObjects = [undefined, null, true, false, 0, 42, -0, NaN, "", "abc", Symbol()];
    if (typeof BigInt === "function") {
        nonObjects.push(BigInt(7));
    }

    var badOptions = {
        get mode() { throw new ExpectedError("mode-should-not-run"); },
        get padding() { throw new ExpectedError("padding-should-not-run"); }
    };

    for (var i = 0; i < nonObjects.length; i++) {
        (function (arg) {
            assertThrows(function () { Iterator.zip(arg, badOptions); }, TypeError, "zip non-obj iterables");
            assertThrows(function () { Iterator.zipKeyed(arg, badOptions); }, TypeError, "zipKeyed non-obj iterables");
        })(nonObjects[i]);
    }

    var invalidOptions = [null, true, false, 0, "", "shortest", Symbol()];
    if (typeof BigInt === "function") {
        invalidOptions.push(BigInt(1));
    }
    for (var i = 0; i < invalidOptions.length; i++) {
        (function (opt) {
            assertThrows(function () { Iterator.zip([], opt); }, TypeError, "zip invalid options");
            assertThrows(function () { Iterator.zipKeyed({}, opt); }, TypeError, "zipKeyed invalid options");
        })(invalidOptions[i]);
    }

    var badModeObj = {
        toString: function () { throw new ExpectedError("no-toString"); },
        valueOf: function () { throw new ExpectedError("no-valueOf"); }
    };
    badModeObj[Symbol.toPrimitive] = function () { throw new ExpectedError("no-toPrimitive"); };
    assertThrows(function () { Iterator.zip([], { mode: badModeObj }); }, TypeError, "zip object mode no coerce");
    assertThrows(function () { Iterator.zipKeyed({}, { mode: badModeObj }); }, TypeError, "zipKeyed object mode no coerce");
    assertThrows(function () { Iterator.zip([], { mode: new String("shortest") }); }, TypeError, "zip boxed String mode");
    assertThrows(function () { Iterator.zipKeyed({}, { mode: new String("longest") }); }, TypeError, "zipKeyed boxed String mode");
    assertThrows(function () { Iterator.zip([], { mode: "short" }); }, TypeError, "zip unknown mode");

    // padding is read ONLY when mode === "longest", and after mode is validated.
    var orderLog = [];
    var iterablesWithLog = {};
    iterablesWithLog[Symbol.iterator] = function () {
        orderLog.push("open-iterables");
        return [].values();
    };
    Iterator.zip(iterablesWithLog, {
        get mode() { orderLog.push("get-mode"); return "longest"; },
        get padding() { orderLog.push("get-padding"); return []; }
    });
    assertArrayEquals(orderLog, ["get-mode", "get-padding", "open-iterables"], "zip option read order");

    orderLog = [];
    Iterator.zip(iterablesWithLog, {
        get mode() { orderLog.push("get-mode"); return undefined; },
        get padding() { orderLog.push("unexpected-padding"); return []; }
    });
    assertArrayEquals(orderLog, ["get-mode", "open-iterables"], "zip undefined mode skips padding");

    // Non-iterable iterables throws; primitive elements throw; boxed String accepted.
    assertThrows(function () { Iterator.zip(Object.create(null)); }, TypeError, "zip non-iterable");
    assertThrows(function () { Iterator.zip(["abc"]); }, TypeError, "zip rejects string primitive element");
    assertThrows(function () { Iterator.zipKeyed({ a: "abc" }); }, TypeError, "zipKeyed rejects string primitive");
    var zippedStr = Array.from(Iterator.zip([new String("ab"), Object("12")]));
    assertSame(zippedStr.length, 2, "boxed string length");
    assertArrayEquals(zippedStr[0], ["a", "1"], "boxed string [0]");
    assertArrayEquals(zippedStr[1], ["b", "2"], "boxed string [1]");
})();

// 3. Modes (shortest, longest, strict) and padding for Iterator.zip
(function testZipModes() {
    // shortest (default)
    var s = Array.from(Iterator.zip([[1, 2, 3], [4, 5]]));
    assertSame(s.length, 2, "shortest count");
    assertArrayEquals(s[0], [1, 4], "shortest[0]");
    assertArrayEquals(s[1], [2, 5], "shortest[1]");

    // longest with iterator padding & close when padding isn't exhausted
    var padClosed = 0;
    var padObj = {};
    padObj[Symbol.iterator] = function () {
        var i = 0;
        return {
            next: function () { return { value: "P" + i++, done: false }; },
            return: function () { padClosed++; return {}; }
        };
    };
    var l = Array.from(Iterator.zip([[1], [2, 3, 4], []], { mode: "longest", padding: padObj }));
    assertSame(padClosed, 1, "padding iterator closed when not exhausted");
    assertSame(l.length, 3, "longest count");
    assertArrayEquals(l[0], [1, 2, "P2"], "longest[0]");
    assertArrayEquals(l[1], ["P0", 3, "P2"], "longest[1]");
    assertArrayEquals(l[2], ["P0", 4, "P2"], "longest[2]");

    // empty iterables still opens and closes padding iterator
    padClosed = 0;
    var emptyLong = Iterator.zip([], { mode: "longest", padding: padObj });
    assertSame(padClosed, 1, "padding iterator opened and closed for empty iterables");
    assertSame(emptyLong.next().done, true, "empty zip done");

    // strict: equal length succeeds, mismatched throws TypeError and closes remaining in reverse
    var eq = Array.from(Iterator.zip([[1, 2], [3, 4]], { mode: "strict" }));
    assertSame(eq.length, 2, "strict equal length");
    assertArrayEquals(eq[1], [2, 4], "strict[1]");

    var strictLog = [];
    var st0 = {
        c: 0,
        next: function () { strictLog.push("n0"); return { value: 1, done: this.c++ > 0 }; },
        return: function () { strictLog.push("r0"); return {}; }
    };
    var st1 = {
        next: function () { strictLog.push("n1"); return { value: 2, done: false }; },
        return: function () { strictLog.push("r1"); return {}; }
    };
    var st2 = {
        next: function () { strictLog.push("n2"); return { value: 3, done: false }; },
        return: function () { strictLog.push("r2"); return {}; }
    };
    var stIt = Iterator.zip([st0, st1, st2], { mode: "strict" });
    assertArrayEquals(stIt.next().value, [1, 2, 3], "strict step 0");
    assertThrows(function () { stIt.next(); }, TypeError, "strict mismatch throws");
    assertArrayEquals(strictLog, ["n0", "n1", "n2", "n0", "n1", "r2", "r1"], "strict i=0 closes k..end in reverse");
})();

// 4. Iterator.zipKeyed: keys, null-prototype, __proto__, symbols, numeric keys, undefined values, padding
(function testZipKeyed() {
    var sym = Symbol("symKey");
    var proto = { inherited: [99] };
    var iterables = Object.create(proto);
    Object.defineProperty(iterables, "nonEnum", {
        enumerable: false,
        get: function () { fail("non-enumerable getter must not be invoked"); }
    });
    iterables.a = [1, 2];
    iterables.undef = undefined;
    Object.defineProperty(iterables, "getterDel", {
        enumerable: true,
        configurable: true,
        get: function () {
            delete iterables.deletedLater;
            return [10];
        }
    });
    iterables.deletedLater = {
        next: function () { fail("deleted property should not be visited"); }
    };
    iterables[5] = [100, 200];
    Object.defineProperty(iterables, "__proto__", {
        value: ["p0", "p1"],
        writable: true,
        enumerable: true,
        configurable: true
    });
    iterables[sym] = ["s0"];

    var paddingObj = { 5: "p5", a: "pa", getterDel: "pg", [sym]: "ps" };
    Object.defineProperty(paddingObj, "__proto__", {
        value: "pp",
        writable: true,
        enumerable: true,
        configurable: true
    });
    var it = Iterator.zipKeyed(iterables, {
        mode: "longest",
        padding: paddingObj
    });

    var r0 = it.next();
    assertSame(r0.done, false, "zipKeyed r0 done");
    assertSame(Object.getPrototypeOf(r0.value), null, "zipKeyed result null prototype");
    assertTrue(Object.isExtensible(r0.value), "zipKeyed result extensible");
    assertSame("inherited" in r0.value, false, "no inherited");
    assertSame("nonEnum" in r0.value, false, "no nonEnum");
    assertSame("undef" in r0.value, false, "no undefined-valued property");
    assertSame("deletedLater" in r0.value, false, "no deleted property");

    var expectedKeys = ["5", "a", "getterDel", "__proto__", sym];
    var actualKeys = Reflect.ownKeys(r0.value);
    assertSame(actualKeys.length, expectedKeys.length, "zipKeyed ownKeys length");
    for (var i = 0; i < expectedKeys.length; i++) {
        assertSame(actualKeys[i], expectedKeys[i], "zipKeyed key[" + i + "]");
        var desc = Object.getOwnPropertyDescriptor(r0.value, expectedKeys[i]);
        assertSame(desc.writable, true, "prop writable");
        assertSame(desc.enumerable, true, "prop enumerable");
        assertSame(desc.configurable, true, "prop configurable");
    }
    assertSame(r0.value[5], 100, "r0[5]");
    assertSame(r0.value.a, 1, "r0.a");
    assertSame(r0.value.getterDel, 10, "r0.getterDel");
    assertSame(r0.value["__proto__"], "p0", "r0.__proto__ own data prop");
    assertSame(r0.value[sym], "s0", "r0[sym]");

    var r1 = it.next();
    assertSame(r1.done, false, "zipKeyed r1 done");
    assertTrue(r1.value !== r0.value, "fresh object each step");
    assertSame(r1.value[5], 200, "r1[5]");
    assertSame(r1.value.a, 2, "r1.a");
    assertSame(r1.value.getterDel, "pg", "r1.getterDel padded");
    assertSame(r1.value["__proto__"], "p1", "r1.__proto__");
    assertSame(r1.value[sym], "ps", "r1[sym] padded");

    assertSame(it.next().done, true, "zipKeyed exhausted");
})();

// 5. Reverse close, abrupt completion preservation, suspended-start vs suspended-yield return(), reentrancy
(function testCloseAndReentrancy() {
    // 5a. Setup GetIteratorFlattenable throws -> closes collected in reverse, then inputIter
    var log = [];
    var mkClosable = function (name) {
        return {
            next: function () { return { value: name, done: false }; },
            return: function () { log.push("close-" + name); throw new Error("ignored"); }
        };
    };
    var badInput = {};
    badInput[Symbol.iterator] = function () {
        var idx = 0;
        var items = [mkClosable("a"), mkClosable("b"), 42];
        return {
            next: function () {
                return idx < items.length ? { value: items[idx++], done: false } : { done: true };
            },
            return: function () {
                log.push("close-input");
                throw new Error("ignored-input");
            }
        };
    };
    assertThrows(function () { Iterator.zip(badInput); }, TypeError, "setup element primitive throws");
    assertArrayEquals(log, ["close-b", "close-a", "close-input"], "setup close reverse + inputIter last");

    // 5b. Suspended-start return() marks completed first, reentrant next/return returns done, preserves first error
    log = [];
    var suspIt;
    var ss0 = {
        next: function () { fail("ss0 next not called"); },
        return: function () {
            log.push("ss0-return");
            var r = suspIt.next();
            assertSame(r.done, true, "reentrant next in suspended-start return is done");
            throw new ExpectedError("ignored-ss0");
        }
    };
    var ss1 = {
        next: function () { fail("ss1 next not called"); },
        return: function () {
            log.push("ss1-return");
            var r = suspIt.return();
            assertSame(r.done, true, "reentrant return in suspended-start return is done");
            throw new ExpectedError("first-ss1");
        }
    };
    suspIt = Iterator.zip([ss0, ss1]);
    var thrown = assertThrows(function () { suspIt.return(); }, ExpectedError, "suspended-start preserves first error");
    assertSame(thrown.tag, "first-ss1", "suspended-start first error tag");
    assertArrayEquals(log, ["ss1-return", "ss0-return"], "suspended-start reverse order");
    assertSame(suspIt.next().done, true, "completed after suspended-start return");

    // 5c. Suspended-yield return() sets executing so reentrant next()/return() throws TypeError
    log = [];
    var syIt;
    var sy0 = {
        next: function () { return { value: 1, done: false }; },
        return: function () {
            log.push("sy0-return");
            assertThrows(function () { syIt.next(); }, TypeError, "reentrant next during suspended-yield return");
            return {};
        }
    };
    var sy1 = {
        next: function () { return { value: 2, done: false }; },
        return: function () {
            log.push("sy1-return");
            assertThrows(function () { syIt.return(); }, TypeError, "reentrant return during suspended-yield return");
            throw new ExpectedError("sy1-err");
        }
    };
    syIt = Iterator.zipKeyed({ a: sy0, b: sy1 });
    assertSame(syIt.next().value.a, 1, "syIt step 0");
    var syErr = assertThrows(function () { syIt.return(); }, ExpectedError, "syIt return rethrows");
    assertSame(syErr.tag, "sy1-err", "syIt first close error preserved");
    assertArrayEquals(log, ["sy1-return", "sy0-return"], "suspended-yield reverse order");
})();

// 6. GC pressure & JIT warmup across many iterations
(function testGcAndJitWarmup() {
    var total = 0;
    for (var round = 0; round < 60; round++) {
        var sym = Symbol("dyn_" + round);
        var keyed = {};
        keyed["k" + (round % 5)] = [round, round + 1];
        keyed[sym] = [round * 2];

        var z = Iterator.zip([[round, round + 1], [10, 20, 30]], { mode: "longest", padding: [99] });
        var zk = Iterator.zipKeyed(keyed, { mode: "longest", padding: { [sym]: 77 } });
        gcNow();

        var a = z.next();
        var b = zk.next();
        gcNow();
        total += a.value[0] + a.value[1] + b.value[sym];

        var a2 = z.next();
        var b2 = zk.next();
        total += a2.value[0] + b2.value[sym];

        var a3 = z.next();
        assertSame(a3.value[0], 99, "jit zip pad");
        assertSame(a3.value[1], 30, "jit zip tail");
        assertSame(z.next().done, true, "jit zip done");
        assertSame(zk.next().done, true, "jit zipKeyed done");
    }
    assertTrue(total > 0, "jit warmup sum");
})();

// 7. Cross-realm iterators if WScript.LoadScript is available
(function testCrossRealm() {
    if (typeof WScript !== "undefined" && typeof WScript.LoadScript === "function") {
        var other = WScript.LoadScript("var arr1 = [10, 20]; var arr2 = ['x', 'y']; var obj = { p: [1, 2], q: [3] };", "samethread");
        var res = Array.from(Iterator.zip([other.arr1, other.arr2]));
        assertSame(res.length, 2, "cross-realm zip length");
        assertArrayEquals(res[0], [10, "x"], "cross-realm zip[0]");
        assertArrayEquals(res[1], [20, "y"], "cross-realm zip[1]");

        var resK = Array.from(Iterator.zipKeyed(other.obj, { mode: "longest", padding: { q: 99 } }));
        assertSame(resK.length, 2, "cross-realm zipKeyed length");
        assertSame(resK[0].p, 1, "cross-realm zipKeyed[0].p");
        assertSame(resK[0].q, 3, "cross-realm zipKeyed[0].q");
        assertSame(resK[1].p, 2, "cross-realm zipKeyed[1].p");
        assertSame(resK[1].q, 99, "cross-realm zipKeyed[1].q");
    }
})();

print("PASS");
