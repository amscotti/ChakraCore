//---------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//---------------------------------------------------------------------------------------------------------
// Q1.5 Wave 1 Slice 1: ES2025 conformance tests for Array & TypedArray plus JIT warmup and GC checks.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function maybeGC() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

function makeNonConfigurableSpeciesArray(idx) {
    class NonConfigSpecies extends Array {
        constructor(...args) {
            super(...args);
            Object.defineProperty(this, idx, { value: 99, configurable: false, writable: true });
        }
    }
    return NonConfigSpecies;
}

function makeNonWritableSpeciesArray(idx) {
    class NonWritableSpecies extends Array {
        constructor(...args) {
            super(...args);
            Object.defineProperty(this, idx, { value: 99, configurable: false, writable: false });
        }
    }
    return NonWritableSpecies;
}

var tests = [
    {
        name: "concat/map/slice CreateDataPropertyOrThrow throws TypeError on non-configurable or non-writable target property",
        body: function () {
            const NonCfg = makeNonConfigurableSpeciesArray(0);
            const NonWrt = makeNonWritableSpeciesArray(0);

            for (let i = 0; i < 20; i++) {
                const a1 = [1];
                a1.constructor = { [Symbol.species]: NonCfg };
                assert.throws(() => a1.concat([2]), TypeError, "concat non-configurable");
                assert.throws(() => a1.map(x => x), TypeError, "map non-configurable");
                assert.throws(() => a1.slice(0), TypeError, "slice non-configurable");

                const a2 = [1];
                a2.constructor = { [Symbol.species]: NonWrt };
                assert.throws(() => a2.concat([2]), TypeError, "concat non-writable");
                assert.throws(() => a2.map(x => x), TypeError, "map non-writable");
                assert.throws(() => a2.slice(0), TypeError, "slice non-writable");
            }

            // Spreadable concat on non-configurable target slot and mid-callback type transition
            const NonCfgSpreadable = makeNonConfigurableSpeciesArray(1);
            const src = [10, 20];
            src.constructor = { [Symbol.species]: NonCfgSpreadable };
            assert.throws(() => src.concat([30]), TypeError, "concat spreadable non-configurable");

            // Map and slice create own data property instead of triggering prototype setter
            let setterHit = false;
            try {
                Object.defineProperty(Array.prototype, 0, {
                    configurable: true,
                    set() { setterHit = true; }
                });
                const m = [1].map(x => x + 1);
                const s = [2].slice(0);
                assert.areEqual(2, m[0]);
                assert.areEqual(2, s[0]);
                assert.isFalse(setterHit, "CreateDataPropertyOrThrow must not invoke prototype setter");
            } finally {
                delete Array.prototype[0];
            }
        }
    },
    {
        name: "Array.from closes iterator on constructor failure and uses DefineOwnProperty semantics",
        body: function () {
            for (let i = 0; i < 20; i++) {
                let getterRead = false;
                let opened = false;
                let closed = false;
                const iterable = {
                    get [Symbol.iterator]() {
                        getterRead = true;
                        return function () {
                            opened = true;
                            return {
                                next() { return { value: 1, done: false }; },
                                return() { closed = true; return { done: true }; }
                            };
                        };
                    }
                };
                function ThrowCtor() {
                    throw new RangeError("ctor failed");
                }
                assert.throws(() => Array.from.call(ThrowCtor, iterable), RangeError);
                assert.isTrue(getterRead, "GetMethod(@@iterator) must run before Construct(C)");
                assert.isFalse(opened, "Iterator must not be opened before Construct(C) throws");

                closed = false;
                function NonWritableTarget() {
                    return Object.defineProperty({}, 0, { value: 7, writable: false, configurable: false });
                }
                assert.throws(() => Array.from.call(NonWritableTarget, iterable), TypeError);
                assert.isTrue(closed, "Array.from iterator must be closed when CreateDataPropertyOrThrow fails");

                assert.throws(() => Array.from.call(NonWritableTarget, { length: 1, 0: 42 }), TypeError);
            }
        }
    },
    {
        name: "Array.of defines own data properties without invoking prototype setters and throws on non-configurable slots",
        body: function () {
            for (let i = 0; i < 20; i++) {
                let called = false;
                try {
                    Object.defineProperty(Array.prototype, 0, {
                        configurable: true,
                        set() { called = true; }
                    });
                    const out = Array.of(10, 20);
                    assert.areEqual(10, out[0]);
                    assert.areEqual(20, out[1]);
                    assert.isFalse(called, "Array.of must not invoke [[Set]]");
                } finally {
                    delete Array.prototype[0];
                }

                function NonConfigCtor() {
                    return Object.defineProperty({}, 0, { value: 1, writable: true, configurable: false });
                }
                assert.throws(() => Array.of.call(NonConfigCtor, 99), TypeError);
            }
        }
    },
    {
        name: "findLast/findLastIndex handle lengths > 2^32 - 1 up to 2^53 - 1 and mid-loop array transitions",
        body: function () {
            const MAX_SAFE = 9007199254740991;
            const obj = {
                length: MAX_SAFE,
                [MAX_SAFE - 1]: "last",
                get [MAX_SAFE - 2]() {
                    throw new Error("must not reach index MAX_SAFE - 2 after match");
                }
            };
            for (let i = 0; i < 20; i++) {
                assert.areEqual("last", Array.prototype.findLast.call(obj, v => v === "last"));
                assert.areEqual(MAX_SAFE - 1, Array.prototype.findLastIndex.call(obj, v => v === "last"));

                // NativeIntArray -> Var array mid-loop transition during reverse scan
                const arr = [1, 2, 3];
                const seen = [];
                const found = arr.findLast((v, idx, a) => {
                    seen.push(idx + ":" + v);
                    if (idx === 2) {
                        a[0] = "mutated";
                    }
                    return v === "mutated";
                });
                assert.areEqual("mutated", found);
                assert.areEqual("2:3,1:2,0:mutated", seen.join(","));

                const arr2 = [1, 2, 3];
                assert.areEqual(0, arr2.findLastIndex((v, idx, a) => {
                    if (idx === 2) {
                        Object.defineProperty(a, 1, { configurable: true, get() { return 42; } });
                    }
                    return v === 1;
                }));
            }
        }
    },
    {
        name: "Array.prototype.slice supports lengths > UINT32_MAX including Proxy receivers",
        body: function () {
            const HIGH = 4294967296; // 2^32
            const obj = { length: HIGH + 4 };
            obj[HIGH + 1] = "a";
            obj[HIGH + 2] = "b";

            const traps = [];
            const proxy = new Proxy(obj, {
                has(target, prop) { traps.push("has:" + String(prop)); return Reflect.has(target, prop); },
                get(target, prop, recv) { traps.push("get:" + String(prop)); return Reflect.get(target, prop, recv); }
            });

            for (let i = 0; i < 20; i++) {
                traps.length = 0;
                const sliced = Array.prototype.slice.call(proxy, HIGH, HIGH + 4);
                assert.areEqual(4, sliced.length);
                assert.isFalse(0 in sliced);
                assert.areEqual("a", sliced[1]);
                assert.areEqual("b", sliced[2]);
                assert.isFalse(3 in sliced);
                assert.isTrue(traps.includes("has:" + (HIGH + 1)));
                assert.isTrue(traps.includes("get:" + (HIGH + 2)));

                const empty = Array.prototype.slice.call({ length: 9007199254740991 }, 9007199254740991);
                assert.areEqual(0, empty.length);
            }
        }
    },
    {
        name: "lastIndexOf checks HasProperty before GetProperty on prototype when length is zeroed",
        body: function () {
            for (let i = 0; i < 20; i++) {
                const calls = [];
                const proto = new Proxy({}, {
                    has(t, k) { calls.push("has:" + String(k)); return Reflect.has(t, k); },
                    get(t, k, r) { calls.push("get:" + String(k)); return Reflect.get(t, k, r); }
                });
                const arr = [10, 20, 30];
                Object.setPrototypeOf(arr, proto);
                const res = Array.prototype.lastIndexOf.call(arr, 10, {
                    valueOf() {
                        arr.length = 0;
                        return 2;
                    }
                });
                assert.areEqual(-1, res);
                // After length is zeroed, empty indices check HasProperty only and never GetProperty.
                assert.areEqual("has:2,has:1,has:0", calls.join(","));
            }
        }
    },
    {
        name: "pop throws TypeError on frozen arrays and non-writable length",
        body: function () {
            for (let i = 0; i < 20; i++) {
                const frozen = Object.freeze([1, 2]);
                assert.throws(() => frozen.pop(), TypeError, "pop on frozen non-empty array");
                assert.areEqual(2, frozen.length);

                const frozenEmpty = Object.freeze([]);
                assert.throws(() => frozenEmpty.pop(), TypeError, "pop on frozen empty array");

                const nw = [1, 2];
                Object.defineProperty(nw, "length", { writable: false });
                assert.throws(() => nw.pop(), TypeError, "pop on array with non-writable length");
                assert.areEqual(2, nw.length);
                assert.areEqual(undefined, nw[1], "pop deletes element before failing to write length");
            }
        }
    },
    {
        name: "sort uses ToLength clamping negative lengths to 0 without throwing",
        body: function () {
            for (let i = 0; i < 20; i++) {
                const obj = { 0: "z", 1: "y", 2: "x", length: -4294967294 };
                assert.areEqual(obj, Array.prototype.sort.call(obj));
                assert.areEqual("z", obj[0]);
                assert.areEqual("y", obj[1]);
                assert.areEqual("x", obj[2]);
            }
        }
    },
    {
        name: "unshift throws TypeError on read-only getter or non-writable index without breaking length semantics",
        body: function () {
            for (let i = 0; i < 20; i++) {
                assert.throws(() => Array.prototype.unshift.call({ get 0() {} }, 0), TypeError);

                const protoGetter = { get 1() {} };
                const o = Object.create(protoGetter);
                o[0] = "a";
                o.length = 1;
                assert.throws(() => Array.prototype.unshift.call(o, "b"), TypeError);

                const nw = {};
                Object.defineProperty(nw, 0, { value: 1, writable: false, configurable: true });
                assert.throws(() => Array.prototype.unshift.call(nw, 2), TypeError);
            }
        }
    },
    {
        name: "Array length setter coerces value before checking [[Writable]]",
        body: function () {
            for (let i = 0; i < 20; i++) {
                const a1 = [1, 2, 3];
                const hints = [];
                const lenObj = {
                    [Symbol.toPrimitive](hint) {
                        hints.push(hint);
                        Object.defineProperty(a1, "length", { writable: false });
                        return 0;
                    }
                };
                assert.throws(() => {
                    "use strict";
                    a1.length = lenObj;
                }, TypeError);
                assert.areEqual("number,number", hints.join(","));
                assert.areEqual(3, a1.length);

                const a2 = [1, 2, 3];
                const hints2 = [];
                const lenObj2 = {
                    [Symbol.toPrimitive](hint) {
                        hints2.push(hint);
                        Object.defineProperty(a2, "length", { writable: false });
                        return 1;
                    }
                };
                assert.isFalse(Reflect.set(a2, "length", lenObj2));
                assert.areEqual("number,number", hints2.join(","));
                assert.areEqual(3, a2.length);
            }
        }
    },
    {
        name: "TypedArray.prototype.sort orders -0 before +0 across float types",
        body: function () {
            for (let i = 0; i < 20; i++) {
                const f64 = new Float64Array([+0, -0, NaN, -Infinity, 1, -0]);
                f64.sort();
                assert.areEqual(-Infinity, f64[0]);
                assert.isTrue(Object.is(f64[1], -0), "f64[1] should be -0");
                assert.isTrue(Object.is(f64[2], -0), "f64[2] should be -0");
                assert.isTrue(Object.is(f64[3], +0), "f64[3] should be +0");
                assert.areEqual(1, f64[4]);
                assert.isTrue(Number.isNaN(f64[5]));

                const f32 = new Float32Array([0, -0]);
                f32.sort();
                assert.isTrue(Object.is(f32[0], -0));
                assert.isTrue(Object.is(f32[1], +0));

                if (typeof Float16Array === "function") {
                    const f16 = new Float16Array([0, -0]);
                    f16.sort();
                    assert.isTrue(Object.is(f16[0], -0));
                    assert.isTrue(Object.is(f16[1], +0));
                }
            }
        }
    },
    {
        name: "TypedArray constructor snapshots iterable before ToNumber mutations and rolls back GuestArenaScratch across GC",
        body: function () {
            function runIterCase(triggerGC) {
                const arr = [null, 2, 3];
                arr[0] = {
                    valueOf() {
                        arr[1] = 200;
                        arr[2] = 300;
                        if (triggerGC) {
                            maybeGC();
                        }
                        return 10;
                    }
                };
                const ta = new Int32Array(arr);
                assert.areEqual("10,2,3", Array.from(ta).join(","));
            }

            // Warm up JIT and trigger GC mid-ToNumber
            for (let i = 0; i < 40; i++) {
                runIterCase(i % 10 === 0);
            }

            // Exercise dense NativeIntArray fast-path correctness as well
            for (let i = 0; i < 40; i++) {
                const dense = [4, 5, 6];
                const ta = new Int32Array(dense);
                assert.areEqual("4,5,6", Array.from(ta).join(","));
            }

            // Exercise GuestArenaScratch RAII release on abrupt completion across GC cycles
            for (let i = 0; i < 50; i++) {
                const throwingSource = [
                    { valueOf() { return 1; } },
                    {
                        valueOf() {
                            if (i % 5 === 0) {
                                maybeGC();
                            }
                            throw new RangeError("abrupt valueOf");
                        }
                    },
                    3
                ];
                assert.throws(() => new Float64Array(throwingSource), RangeError);
            }
            maybeGC();
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
