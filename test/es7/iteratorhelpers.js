//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// A closable iterator over `values` with call counters, in the shape
// of test262's TestIterator. It inherits from Iterator so the helpers
// are directly available.
function makeIterator(values) {
    let index = 0;
    const it = {
        nextCalls: 0,
        returnCalls: 0,
        next() {
            it.nextCalls++;
            return index < values.length
                ? { value: values[index++], done: false }
                : { value: undefined, done: true };
        },
        return() {
            it.returnCalls++;
            index = values.length;
            return { value: undefined, done: true };
        },
    };
    Object.setPrototypeOf(it, Iterator.prototype);
    return it;
}

function* gen123() {
    yield 1;
    yield 2;
    yield 3;
}

var tests = [
    {
        name: "Iterator constructor is abstract and subclassable",
        body() {
            assert.areEqual("function", typeof Iterator);
            assert.areEqual(0, Iterator.length);
            assert.areEqual("Iterator", Iterator.name);
            assert.throws(() => Iterator(), TypeError, "call without new");
            assert.throws(() => new Iterator(), TypeError, "new Iterator()");
            assert.throws(() => Reflect.construct(Iterator, [], Iterator), TypeError, "newTarget is Iterator");

            class Sub extends Iterator {}
            const sub = new Sub();
            assert.isTrue(sub instanceof Iterator, "subclass instance is an Iterator");
            assert.isTrue(sub instanceof Sub, "subclass instance is a Sub");

            const fromNewTarget = Reflect.construct(Iterator, [], Sub);
            assert.isTrue(Object.getPrototypeOf(fromNewTarget) === Sub.prototype, "prototype from newTarget");
        }
    },
    {
        name: "Built-in descriptors, lengths and names",
        body() {
            // Runs before the later test that freezes Iterator.prototype.
            function assertData(obj, key, expected, message) {
                const desc = Object.getOwnPropertyDescriptor(obj, key);
                assert.isTrue(desc !== undefined, message + " exists");
                assert.areEqual(expected.writable, desc.writable, message + " writable");
                assert.areEqual(expected.enumerable, desc.enumerable, message + " enumerable");
                assert.areEqual(expected.configurable, desc.configurable, message + " configurable");
                if (Object.prototype.hasOwnProperty.call(expected, "value")) {
                    assert.areEqual(expected.value, desc.value, message + " value");
                }
            }

            assertData(Iterator, "length", { value: 0, writable: false, enumerable: false, configurable: true }, "Iterator.length");
            assertData(Iterator, "name", { value: "Iterator", writable: false, enumerable: false, configurable: true }, "Iterator.name");
            assertData(Iterator, "prototype", { value: Iterator.prototype, writable: false, enumerable: false, configurable: false }, "Iterator.prototype");
            assertData(Iterator, "from", { writable: true, enumerable: false, configurable: true }, "Iterator.from");
            assert.areEqual(1, Iterator.from.length, "from length");
            assert.areEqual("from", Iterator.from.name, "from name");

            const methods = {
                map: 1, filter: 1, take: 1, drop: 1, flatMap: 1, reduce: 1,
                toArray: 0, forEach: 1, some: 1, every: 1, find: 1,
            };
            for (const name in methods) {
                assertData(Iterator.prototype, name, { writable: true, enumerable: false, configurable: true }, "Iterator.prototype." + name);
                assert.areEqual(methods[name], Iterator.prototype[name].length, name + " length");
                assert.areEqual(name, Iterator.prototype[name].name, name + " name");
            }

            assertData(Iterator.prototype, Symbol.iterator, { writable: true, enumerable: false, configurable: true }, "@@iterator");
            assert.areEqual(0, Iterator.prototype[Symbol.iterator].length, "@@iterator length");
            assert.areEqual("[Symbol.iterator]", Iterator.prototype[Symbol.iterator].name, "@@iterator name");

            const ctorDesc = Object.getOwnPropertyDescriptor(Iterator.prototype, "constructor");
            assert.areEqual("function", typeof ctorDesc.get, "constructor getter");
            assert.areEqual("function", typeof ctorDesc.set, "constructor setter");
            assert.areEqual(false, ctorDesc.enumerable, "constructor enumerable");
            assert.areEqual(true, ctorDesc.configurable, "constructor configurable");

            const tagDesc = Object.getOwnPropertyDescriptor(Iterator.prototype, Symbol.toStringTag);
            assert.areEqual("function", typeof tagDesc.get, "toStringTag getter");
            assert.areEqual("function", typeof tagDesc.set, "toStringTag setter");
            assert.areEqual(false, tagDesc.enumerable, "toStringTag enumerable");
            assert.areEqual(true, tagDesc.configurable, "toStringTag configurable");

            const helper = makeIterator([1]).map(x => x);
            const helperProto = Object.getPrototypeOf(helper);
            assertData(helperProto, "next", { writable: true, enumerable: false, configurable: true }, "helper next");
            assert.areEqual(0, helperProto.next.length, "helper next length");
            assert.areEqual("next", helperProto.next.name, "helper next name");
            assert.areEqual(0, helperProto.return.length, "helper return length");
            assert.areEqual("return", helperProto.return.name, "helper return name");
            assertData(helperProto, Symbol.toStringTag, {
                value: "Iterator Helper", writable: false, enumerable: false, configurable: true,
            }, "helper toStringTag");

            const wrapped = Iterator.from({ next() { return { done: true, value: undefined }; } });
            const wrapProto = Object.getPrototypeOf(wrapped);
            assert.areEqual(0, wrapProto.next.length, "wrap next length");
            assert.areEqual("next", wrapProto.next.name, "wrap next name");
            assert.areEqual(0, wrapProto.return.length, "wrap return length");
            assert.areEqual("return", wrapProto.return.name, "wrap return name");
            assert.isTrue(Object.getOwnPropertyDescriptor(wrapProto, Symbol.toStringTag) === undefined, "wrap proto has no own toStringTag");
        }
    },
    {
        name: "Iterator.prototype identity, constructor and toStringTag accessors",
        body() {
            const iterProto = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));
            assert.isTrue(Iterator.prototype === iterProto, "Iterator.prototype is %IteratorPrototype%");
            assert.isTrue(Object.getPrototypeOf(Iterator.prototype) === Object.prototype, "proto is Object.prototype");
            assert.isTrue(Iterator.prototype.constructor === Iterator, "constructor getter");
            assert.areEqual("Iterator", Iterator.prototype[Symbol.toStringTag]);

            const fake = Object.create(Iterator.prototype);
            Object.freeze(Iterator.prototype);
            fake[Symbol.toStringTag] = "sentinel";
            assert.areEqual("sentinel", fake[Symbol.toStringTag], "weird setter creates own property on receiver");
            assert.throws(() => { "use strict"; Iterator.prototype.constructor = 1; }, TypeError, "setter on home throws");
        }
    },
    {
        name: "Iterator.from cases",
        body() {
            assert.areEqual([1, 2, 3], Array.from(Iterator.from([1, 2, 3])), "from array");
            assert.areEqual(["a", "b"], Array.from(Iterator.from("ab")), "from string primitive");
            assert.throws(() => Iterator.from(5), TypeError, "from number primitive");
            assert.throws(() => Iterator.from(undefined), TypeError, "from undefined");
            assert.throws(() => Iterator.from(null), TypeError, "from null");

            const g = gen123();
            assert.isTrue(Iterator.from(g) === g, "generator is used directly");

            // A plain iterator is wrapped so helpers apply.
            let i = 0;
            const plain = { next() { return i < 2 ? { value: i++, done: false } : { value: undefined, done: true }; } };
            const wrapped = Iterator.from(plain);
            assert.isTrue(wrapped instanceof Iterator, "wrapped iterator is an Iterator");
            assert.areEqual([0, 1], Array.from(wrapped.map(x => x)), "helpers work on wrapped iterator");

            // Iterable whose @@iterator returns a plain iterator.
            i = 0;
            const iterable = { [Symbol.iterator]() { return plain; } };
            i = 0;
            assert.areEqual([0, 1], Array.from(Iterator.from(iterable)), "from iterable returning plain iterator");
        }
    },
    {
        name: "WrapForValidIterator return forwarding",
        body() {
            const sentinelResult = { value: 5, done: true };
            let getNext = 0, getReturn = 0, callReturn = 0;
            const base = {
                get next() { getNext++; return () => ({ value: 1, done: false }); },
                get return() { getReturn++; return () => { callReturn++; return sentinelResult; }; },
            };
            const wrapped = Iterator.from(base);
            assert.areEqual(1, getNext, "from() reads next once");
            assert.areEqual(0, getReturn, "from() does not read return");
            assert.areEqual(0, callReturn, "from() does not call return");
            assert.isTrue(wrapped.return() === sentinelResult, "return() forwards the base result object");
            assert.areEqual(1, getReturn, "return() reads return");
            assert.areEqual(1, callReturn, "return() calls return");

            const wrapProto = Object.getPrototypeOf(wrapped);
            assert.throws(() => wrapProto.next.call({}), TypeError, "wrap next with invalid this");
            assert.throws(() => wrapProto.return.call(5), TypeError, "wrap return with invalid this");
        }
    },
    {
        name: "WrapForValidIterator next forwards every call, including after done",
        body() {
            const doneResult = { value: undefined, done: true };
            let nextCalls = 0;
            const plain = {
                next() {
                    nextCalls++;
                    return nextCalls === 1 ? { value: 7, done: false } : doneResult;
                },
            };
            const wrapped = Iterator.from(plain);
            assert.areEqual(7, wrapped.next().value, "first wrapped value");
            assert.isTrue(wrapped.next() === doneResult, "done result is forwarded unchanged");
            assert.areEqual(2, nextCalls, "underlying next called twice");
            assert.isTrue(wrapped.next() === doneResult, "next after done is still forwarded");
            assert.areEqual(3, nextCalls, "underlying next called again after done");

            // next() does not read "done" and does not require an object.
            let doneGetterCalls = 0;
            const getterResult = {
                value: 5,
                get done() { doneGetterCalls++; return true; },
            };
            const getterWrapped = Iterator.from({ next() { return getterResult; } });
            assert.isTrue(getterWrapped.next() === getterResult, "result object is returned unchanged");
            assert.areEqual(0, doneGetterCalls, "wrap next does not read done");
            assert.areEqual(5, Iterator.from({ next() { return 5; } }).next(), "non-object next result is forwarded");
        }
    },
    {
        name: "Lazy helpers do not pull before next and produce mapped/filtered values",
        body() {
            const it = makeIterator([1, 2, 3, 4]);
            const seen = [];
            const mapped = it.map((v, i) => { seen.push([v, i]); return v * 10; });
            assert.areEqual(0, it.nextCalls, "map is lazy");
            assert.areEqual(10, mapped.next().value);
            assert.areEqual(1, it.nextCalls, "one pull per next");
            assert.areEqual([20, 30, 40], Array.from(mapped), "remaining mapped values");
            assert.areEqual([[1, 0], [2, 1], [3, 2], [4, 3]], seen, "counter starts at 0");

            const it2 = makeIterator([1, 2, 3, 4]);
            const filtered = it2.filter(v => v % 2 === 0);
            assert.areEqual(0, it2.nextCalls, "filter is lazy");
            assert.areEqual([2, 4], Array.from(filtered), "filtered values");

            assert.isTrue(mapped instanceof Iterator, "helper is an Iterator");
            const helperProto = Object.getPrototypeOf(mapped);
            assert.isTrue(Object.getPrototypeOf(helperProto) === Iterator.prototype, "helper prototype chains to Iterator.prototype");
            assert.areEqual("[object Iterator Helper]", Object.prototype.toString.call(mapped), "Iterator Helper toStringTag");
        }
    },
    {
        name: "take and drop limits, truncation and closing",
        body() {
            const it = makeIterator([1, 2, 3, 4]);
            const taken = it.take(2);
            assert.areEqual([1, 2], Array.from(taken), "take(2)");
            assert.areEqual(1, it.returnCalls, "take closes the underlying iterator once");
            assert.isTrue(taken.next().done, "exhausted helper stays done");
            assert.areEqual(1, it.returnCalls, "no second close");

            assert.areEqual([3, 4], Array.from(makeIterator([1, 2, 3, 4]).drop(2)), "drop(2)");
            assert.areEqual([1], Array.from(makeIterator([1, 2, 3]).take(1.9)), "take truncates toward zero");
            assert.areEqual([], Array.from(makeIterator([1]).take(0)), "take(0)");
            assert.areEqual([1, 2], Array.from(makeIterator([1, 2]).take(Infinity)), "take(Infinity)");

            assert.throws(() => makeIterator([1]).take(-1), RangeError, "negative limit");
            assert.throws(() => makeIterator([1]).take(NaN), RangeError, "NaN limit");
            assert.throws(() => makeIterator([1]).drop(NaN), RangeError, "drop(NaN)");
            assert.throws(() => makeIterator([1]).drop(2 ** 53), RangeError, "limit above MAX_SAFE_INTEGER");
        }
    },
    {
        name: "flatMap flattens and closes inner and outer on return",
        body() {
            const lazyOuter = makeIterator([1, 2]);
            let mapperCalls = 0;
            const lazyFlat = lazyOuter.flatMap(v => { mapperCalls++; return [v]; });
            assert.areEqual(0, lazyOuter.nextCalls, "flatMap does not pull before next");
            assert.areEqual(0, mapperCalls, "flatMap does not call the mapper before next");
            assert.areEqual(1, lazyFlat.next().value, "first flatMapped value");
            assert.areEqual(1, mapperCalls, "mapper runs on the first next");
            assert.areEqual(1, lazyOuter.nextCalls, "one outer pull opened the inner");

            assert.areEqual([1, 1, 2, 2, 3, 3], Array.from(gen123().flatMap(v => [v, v])), "flatMap over arrays");
            assert.areEqual([1, 2], Array.from(makeIterator([1, 2]).flatMap(v => makeIterator([v]))), "flatMap over iterators");
            assert.throws(() => Array.from(gen123().flatMap(() => "ab")), TypeError, "strings are not flattened");

            const inner = makeIterator([10, 20]);
            const outer = makeIterator([1]);
            const flat = outer.flatMap(() => inner);
            assert.areEqual(10, flat.next().value, "first inner value");
            flat.return();
            assert.areEqual(1, inner.returnCalls, "return closes the inner iterator");
            assert.areEqual(1, outer.returnCalls, "return closes the outer iterator");
            assert.isTrue(flat.next().done, "helper is done after return");
        }
    },
    {
        name: "Closing exactly once on mapper error, completion and early return",
        body() {
            const it = makeIterator([1, 2, 3]);
            const mapped = it.map(() => { throw new Error("boom"); });
            assert.throws(() => mapped.next(), Error, "mapper error propagates");
            assert.areEqual(1, it.returnCalls, "mapper error closes underlying once");
            assert.isTrue(mapped.next().done, "helper completes after error");
            assert.areEqual(1, it.returnCalls, "still one close");

            const it2 = makeIterator([1, 2, 3]);
            const m2 = it2.map(x => x);
            m2.return();
            assert.areEqual(1, it2.returnCalls, "return before next forwards to underlying");

            const it3 = makeIterator([1]);
            const m3 = it3.map(x => x);
            assert.isTrue(!m3.next().done);
            assert.isTrue(m3.next().done, "exhausted");
            m3.return();
            assert.areEqual(0, it3.returnCalls, "return after exhaustion is not forwarded");
        }
    },
    {
        name: "Validation order and argument errors close the underlying iterator",
        body() {
            // Non-callable mapper: closes (calls return) without reading next.
            let closed = false;
            const closable = {
                get next() { throw new Error("next must not be read"); },
                return() { closed = true; return {}; },
            };
            Object.setPrototypeOf(closable, Iterator.prototype);
            assert.throws(() => closable.map(null), TypeError, "non-callable mapper");
            assert.isTrue(closed, "validation failure closed the iterator");

            assert.throws(() => Iterator.prototype.map.call(undefined, x => x), TypeError, "undefined this");
            assert.throws(() => Iterator.prototype.map.call(5, x => x), TypeError, "primitive this");
            assert.throws(() => Iterator.prototype.take.call({}, "x"), RangeError, "NaN limit from ToNumber");

            // Brand checks on the helper prototype.
            const helper = makeIterator([1]).map(x => x);
            const helperProto = Object.getPrototypeOf(helper);
            assert.throws(() => helperProto.next.call({}), TypeError, "helper next on plain object");
            assert.throws(() => helperProto.next.call(gen123()), TypeError, "helper next on a generator");
            const genProto = Object.getPrototypeOf(Object.getPrototypeOf(gen123()));
            assert.throws(() => genProto.next.call(helper), TypeError, "generator next on a helper");
        }
    },
    {
        name: "Reentrant helper next from a mapper throws TypeError",
        body() {
            let helper;
            let caught;
            function* g() { yield 1; yield 2; }
            helper = g().map(() => {
                try { helper.next(); } catch (e) { caught = e; }
                return 0;
            });
            assert.areEqual(0, helper.next().value);
            assert.isTrue(caught instanceof TypeError, "reentrant next threw TypeError");
        }
    },
    {
        name: "Reentrant next and return during suspended-start close return done results",
        body() {
            // return() before the first next sets the generator to completed
            // and only then closes. Reentry observes completed, not executing.
            let helper;
            let nextResult, returnResult;
            let nextCalls = 0, returnCalls = 0;
            const underlying = {
                next() { nextCalls++; return { value: 1, done: false }; },
                return() {
                    returnCalls++;
                    nextResult = helper.next();
                    returnResult = helper.return();
                    return { value: undefined, done: true };
                },
            };
            Object.setPrototypeOf(underlying, Iterator.prototype);
            helper = underlying.map(x => x);
            const outer = helper.return();
            assert.isTrue(nextResult.done === true && nextResult.value === undefined, "reentrant next during suspended-start close is done");
            assert.isTrue(returnResult.done === true && returnResult.value === undefined, "reentrant return during suspended-start close is done");
            assert.isTrue(outer.done === true && outer.value === undefined, "outer return is done");
            assert.areEqual(0, nextCalls, "suspended-start close does not pull next");
            assert.areEqual(1, returnCalls, "underlying return called exactly once");
            assert.isTrue(helper.next().done, "helper stays done");
            assert.areEqual(1, returnCalls, "still one underlying close");
        }
    },
    {
        name: "Reentrant next and return during a started helper's return throw TypeError",
        body() {
            // After next() the helper is suspended-yield. return() resumes it
            // as executing, so reentry during that close throws. (Node returns
            // a done result here; the spec's GeneratorValidate throws.)
            let helper;
            let nextCaught, returnCaught;
            let returnCalls = 0;
            const underlying = {
                next() { return { value: 1, done: false }; },
                return() {
                    returnCalls++;
                    try { helper.next(); } catch (e) { nextCaught = e; }
                    try { helper.return(); } catch (e) { returnCaught = e; }
                    return { value: undefined, done: true };
                },
            };
            Object.setPrototypeOf(underlying, Iterator.prototype);
            helper = underlying.map(x => x);
            assert.isTrue(!helper.next().done, "start the helper");
            const outer = helper.return();
            assert.isTrue(nextCaught instanceof TypeError, "reentrant next while a started close throws TypeError");
            assert.isTrue(returnCaught instanceof TypeError, "reentrant return while a started close throws TypeError");
            assert.isTrue(outer.done === true, "outer return is done");
            assert.areEqual(1, returnCalls, "underlying return called exactly once");
            assert.isTrue(helper.next().done, "helper stays done");
            assert.areEqual(1, returnCalls, "still one underlying close");
        }
    },
    {
        name: "Reentrant helper calls during error-path closing throw TypeError",
        body() {
            let helper;
            let nextCaught, returnCaught, outerCaught;
            let returnCalls = 0;
            const underlying = {
                next() { return { value: 1, done: false }; },
                return() {
                    returnCalls++;
                    try { helper.next(); } catch (e) { nextCaught = e; }
                    try { helper.return(); } catch (e) { returnCaught = e; }
                    return { value: undefined, done: true };
                },
            };
            Object.setPrototypeOf(underlying, Iterator.prototype);
            helper = underlying.map(() => { throw new Error("boom"); });
            try { helper.next(); } catch (e) { outerCaught = e; }
            assert.areEqual("boom", outerCaught && outerCaught.message, "original mapper error propagates");
            assert.isTrue(nextCaught instanceof TypeError, "reentrant next during swallow-close threw TypeError");
            assert.isTrue(returnCaught instanceof TypeError, "reentrant return during swallow-close threw TypeError");
            assert.areEqual(1, returnCalls, "error path closed underlying exactly once");
            assert.isTrue(helper.next().done, "helper completes after error");
            assert.areEqual(1, returnCalls, "still one underlying close");
        }
    },
    {
        name: "Eager helpers: reduce, toArray, forEach, some, every, find",
        body() {
            assert.areEqual(6, gen123().reduce((a, b) => a + b), "reduce without initial");
            assert.areEqual(16, gen123().reduce((a, b) => a + b, 10), "reduce with initial");
            assert.throws(() => makeIterator([]).reduce((a, b) => a + b), TypeError, "reduce of empty without initial");
            assert.areEqual(5, makeIterator([]).reduce((a, b) => a + b, 5), "reduce of empty with initial");

            assert.areEqual([1, 2, 3], gen123().toArray(), "toArray");

            const visited = [];
            makeIterator([7, 8]).forEach((v, i) => visited.push([v, i]));
            assert.areEqual([[7, 0], [8, 1]], visited, "forEach values and counters");

            const itSome = makeIterator([1, 2, 3]);
            assert.isTrue(itSome.some(v => v === 2), "some finds a match");
            assert.areEqual(1, itSome.returnCalls, "some closes on early exit");
            assert.isFalse(makeIterator([1]).some(v => v === 2), "some without a match");

            const itEvery = makeIterator([2, 4, 5]);
            assert.isFalse(itEvery.every(v => v % 2 === 0), "every fails on the odd value");
            assert.areEqual(1, itEvery.returnCalls, "every closes on early exit");
            assert.isTrue(makeIterator([]).every(() => false), "every on empty is true");

            const itFind = makeIterator([1, 2, 3]);
            assert.areEqual(2, itFind.find(v => v === 2), "find returns the value");
            assert.areEqual(1, itFind.returnCalls, "find closes on success");
            assert.areEqual(undefined, makeIterator([1]).find(v => v === 9), "find without a match");
        }
    },
    {
        name: "Iterator.prototype @@iterator returns this",
        body() {
            const it = makeIterator([1]);
            assert.isTrue(it[Symbol.iterator]() === it, "iterator returns itself");
            const helper = it.map(x => x);
            assert.isTrue(helper[Symbol.iterator]() === helper, "helper returns itself");
            assert.areEqual(4, Iterator.prototype[Symbol.iterator].call(4), "primitive this is returned unchanged");
            assert.areEqual(undefined, Iterator.prototype[Symbol.iterator].call(undefined), "undefined this is returned unchanged");
        }
    },
    {
        name: "Deleted String.prototype @@iterator does not wrap the primitive",
        body() {
            // GetIteratorFlattenable step 5: a missing @@iterator falls back to
            // the argument itself, then throws when that value is not an object.
            // A string primitive is not wrapped.
            const desc = Object.getOwnPropertyDescriptor(String.prototype, Symbol.iterator);
            delete String.prototype[Symbol.iterator];
            try {
                assert.throws(() => Iterator.from("ab"), TypeError, "string primitive with no @@iterator");
                assert.throws(() => Iterator.from(""), TypeError, "empty string primitive with no @@iterator");
            } finally {
                Object.defineProperty(String.prototype, Symbol.iterator, desc);
            }
            assert.areEqual(["a", "b"], Array.from(Iterator.from("ab")), "string iterator restored");
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
