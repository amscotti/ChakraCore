//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

const getters = [
    "getInt8", "getUint8",
    "getInt16", "getUint16",
    "getInt32", "getUint32",
    "getFloat32", "getFloat64"
];

const setters = [
    "setInt8", "setUint8",
    "setInt16", "setUint16",
    "setInt32", "setUint32",
    "setFloat32", "setFloat64"
];

const sizes = [1, 1, 2, 2, 4, 4, 4, 8];

const tests = [
    {
        name: "DataView getters default omitted byteOffset to undefined (0)",
        body: function () {
            const buf = new ArrayBuffer(8);
            const dv = new DataView(buf);
            const bytes = new Uint8Array(buf);
            bytes.set([0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0]);

            for (let i = 0; i < getters.length; i++) {
                const name = getters[i];
                const fromNoArg = dv[name]();
                const fromUndef = dv[name](undefined);
                if (Number.isNaN(fromUndef)) {
                    assert.isTrue(Number.isNaN(fromNoArg), name + "() matches " + name + "(undefined) for NaN");
                } else {
                    assert.areEqual(fromUndef, fromNoArg, name + "() matches " + name + "(undefined)");
                }
            }

            assert.areEqual(0x12, dv.getInt8());
            assert.areEqual(0x12, dv.getUint8());
            assert.areEqual(0x1234, dv.getInt16());
            assert.areEqual(0x1234, dv.getUint16());
            assert.areEqual(0x12345678, dv.getInt32());
            assert.areEqual(0x12345678, dv.getUint32());

            const dvFloat = new DataView(new ArrayBuffer(8));
            dvFloat.setFloat32(0, 1.5);
            assert.areEqual(1.5, dvFloat.getFloat32());
            dvFloat.setFloat64(0, 3.25);
            assert.areEqual(3.25, dvFloat.getFloat64());
        }
    },
    {
        name: "DataView setters default omitted byteOffset and value to undefined",
        body: function () {
            for (let i = 0; i < setters.length; i++) {
                const buf = new ArrayBuffer(8);
                const dv = new DataView(buf);
                const bytes = new Uint8Array(buf);
                bytes.fill(0xff);

                dv[setters[i]]();
                const readBack = dv[getters[i]](0);
                if (setters[i] === "setFloat32" || setters[i] === "setFloat64") {
                    assert.isTrue(Number.isNaN(readBack), setters[i] + "() writes NaN at 0");
                } else {
                    assert.areEqual(0, readBack, setters[i] + "() writes 0 at 0");
                }
                for (let b = sizes[i]; b < 8; b++) {
                    assert.areEqual(0xff, bytes[b], setters[i] + "() does not clobber beyond size");
                }

                bytes.fill(0xaa);
                dv[setters[i]](0);
                const readOne = dv[getters[i]](0);
                if (setters[i] === "setFloat32" || setters[i] === "setFloat64") {
                    assert.isTrue(Number.isNaN(readOne), setters[i] + "(0) writes NaN");
                } else {
                    assert.areEqual(0, readOne, setters[i] + "(0) writes 0");
                }
            }
        }
    },
    {
        name: "DataView GetViewValue / SetViewValue step ordering and range checks",
        body: function () {
            const empty = new DataView(new ArrayBuffer(0));
            const detached = new DataView(new ArrayBuffer(8));
            detached.buffer.transfer();

            for (let i = 0; i < getters.length; i++) {
                assert.throws(() => empty[getters[i]](), RangeError, getters[i] + "() on 0-byte buffer throws RangeError");
                assert.throws(() => detached[getters[i]](), TypeError, getters[i] + "() on detached buffer throws TypeError");
                assert.throws(() => empty[setters[i]](), RangeError, setters[i] + "() on 0-byte buffer throws RangeError");
                assert.throws(() => detached[setters[i]](), TypeError, setters[i] + "() on detached buffer throws TypeError");
                assert.throws(() => detached[getters[i]](Symbol()), TypeError, getters[i] + "(Symbol) throws before detached");
                assert.throws(() => detached[getters[i]](-1), RangeError, getters[i] + "(-1) throws RangeError before detached");
            }

            // SetViewValue converts value before CheckViewRange (detached / OOB).
            for (let i = 0; i < setters.length; i++) {
                let valueConverted = false;
                const badValue = {
                    valueOf: function () {
                        valueConverted = true;
                        throw new Error("value-first");
                    }
                };
                assert.throws(() => detached[setters[i]](0, badValue), Error, setters[i] + " converts value before detached check");
                assert.isTrue(valueConverted, setters[i] + " called valueOf before detached check");
                assert.throws(() => empty[setters[i]](0, Symbol()), TypeError, setters[i] + " Symbol value throws TypeError before RangeError");
            }

            // ToIndex(offset) runs before value conversion on setters.
            const dv = new DataView(new ArrayBuffer(8));
            for (let i = 0; i < setters.length; i++) {
                let valueTouched = false;
                const val = { valueOf: function () { valueTouched = true; return 1; } };
                assert.throws(() => dv[setters[i]](-1, val), RangeError, setters[i] + "(-1) throws RangeError before valueOf");
                assert.isFalse(valueTouched, setters[i] + " did not touch value after invalid offset");
            }
        }
    },
    {
        name: "DataView reentrancy: valueOf detaching or resizing buffer before CheckViewRange",
        body: function () {
            for (let i = 0; i < getters.length; i++) {
                const buf = new ArrayBuffer(8);
                const dv = new DataView(buf);
                assert.throws(() => {
                    dv[getters[i]]({
                        valueOf: function () {
                            buf.transfer();
                            return 0;
                        }
                    });
                }, TypeError, getters[i] + " throws TypeError when offset.valueOf detaches");
            }

            for (let i = 0; i < setters.length; i++) {
                const buf1 = new ArrayBuffer(8);
                const dv1 = new DataView(buf1);
                assert.throws(() => {
                    dv1[setters[i]]({
                        valueOf: function () {
                            buf1.transfer();
                            return 0;
                        }
                    }, 1);
                }, TypeError, setters[i] + " throws TypeError when offset.valueOf detaches");

                const buf2 = new ArrayBuffer(8);
                const dv2 = new DataView(buf2);
                assert.throws(() => {
                    dv2[setters[i]](0, {
                        valueOf: function () {
                            buf2.transfer();
                            return 1;
                        }
                    });
                }, TypeError, setters[i] + " throws TypeError when value.valueOf detaches");
            }

            // Resizable ArrayBuffer: shrink below view offset/length vs shrink below access size vs grow.
            for (let i = 0; i < setters.length; i++) {
                const rbuf = new ArrayBuffer(8, { maxByteLength: 16 });
                const fixed = new DataView(rbuf, 0, 8);
                assert.throws(() => {
                    fixed[setters[i]](0, {
                        valueOf: function () {
                            rbuf.resize(4);
                            return 1;
                        }
                    });
                }, TypeError, setters[i] + " fixed view becomes OOB TypeError after shrink");

                const autoBuf = new ArrayBuffer(8, { maxByteLength: 16 });
                const autoView = new DataView(autoBuf);
                if (sizes[i] > 1) {
                    assert.throws(() => {
                        autoView[setters[i]](0, {
                            valueOf: function () {
                                autoBuf.resize(1);
                                return 1;
                            }
                        });
                    }, RangeError, setters[i] + " auto-length view throws RangeError after shrink below element size");
                }

                const growBuf = new ArrayBuffer(1, { maxByteLength: 16 });
                const growView = new DataView(growBuf);
                growView[setters[i]](0, {
                    valueOf: function () {
                        growBuf.resize(8);
                        return 42;
                    }
                });
                assert.areEqual(42, growView[getters[i]](0), setters[i] + " succeeds after value.valueOf grows buffer");
            }
        }
    },
    {
        name: "Reflect.construct ordering: argArray before newTarget.prototype, built-ins, classes, BoundFunction, cross-realm",
        body: function () {
            function makeThrowingNewTarget() {
                const fn = function () { }.bind(null);
                Object.defineProperty(fn, "prototype", {
                    get: function () {
                        throw new Error("newTarget.prototype accessed");
                    }
                });
                return fn;
            }

            const throwingNewTarget = makeThrowingNewTarget();

            // Built-ins validate before OrdinaryCreateFromConstructor(newTarget).
            assert.throws(() => {
                Reflect.construct(DataView, [new ArrayBuffer(0), 10], throwingNewTarget);
            }, RangeError, "DataView validates byteOffset before newTarget.prototype");

            assert.throws(() => {
                Reflect.construct(ArrayBuffer, [10, { maxByteLength: 0 }], throwingNewTarget);
            }, RangeError, "ArrayBuffer validates maxByteLength before newTarget.prototype");

            assert.throws(() => {
                Reflect.construct(Promise, [], throwingNewTarget);
            }, TypeError, "Promise validates callable executor before newTarget.prototype");

            // CreateListFromArrayLike(argArray) runs before OrdinaryCreateFromConstructor(newTarget).
            function Ordinary() { }
            assert.throws(() => {
                Reflect.construct(Ordinary, {
                    get length() { throw new RangeError("length-first"); }
                }, throwingNewTarget);
            }, RangeError, "argArray.length runs before newTarget.prototype");

            assert.throws(() => {
                Reflect.construct(Ordinary, {
                    length: 1,
                    get 0() { throw new RangeError("elem-first"); }
                }, throwingNewTarget);
            }, RangeError, "argArray[0] runs before newTarget.prototype");

            // Derived class constructor returning an object never invokes newTarget.prototype.
            class Base { }
            class DerivedNoSuper extends Base {
                constructor() {
                    return { ok: true };
                }
            }
            const derivedRes = Reflect.construct(DerivedNoSuper, [], throwingNewTarget);
            assert.isTrue(derivedRes.ok, "derived constructor skips newTarget.prototype without super()");

            // Base class constructor and ordinary constructor read newTarget.prototype once.
            let baseReads = 0;
            const countingTarget = function () { }.bind(null);
            Object.defineProperty(countingTarget, "prototype", {
                get: function () {
                    baseReads++;
                    return Base.prototype;
                }
            });
            class PlainBase { }
            Reflect.construct(PlainBase, [], countingTarget);
            assert.areEqual(1, baseReads, "base class constructor reads newTarget.prototype once");

            let ordReads = 0;
            const ordCountingTarget = function () { }.bind(null);
            Object.defineProperty(ordCountingTarget, "prototype", {
                get: function () {
                    ordReads++;
                    return Ordinary.prototype;
                }
            });
            Reflect.construct(Ordinary, [], ordCountingTarget);
            assert.areEqual(1, ordReads, "ordinary constructor reads newTarget.prototype once");

            // BoundFunction unwrapping and SameValue(boundFn, newTarget) substitution.
            let observedNewTarget;
            let observedArgs;
            function Target(a, b, c) {
                observedNewTarget = new.target;
                observedArgs = [a, b, c];
            }
            const B = Target.bind(null, "a");
            const C = B.bind(null, "b");

            const r1 = Reflect.construct(C, ["c"], C);
            assert.areEqual(Target, observedNewTarget, "Reflect.construct(C, args, C) sets new.target = Target");
            assert.areEqual(["a", "b", "c"], observedArgs, "bound args prepend innermost-first");
            assert.areEqual(Target.prototype, Object.getPrototypeOf(r1));

            const r2 = Reflect.construct(C, ["c"], B);
            assert.areEqual(Target, observedNewTarget, "Reflect.construct(C, args, B) sets new.target = Target");
            assert.areEqual(Target.prototype, Object.getPrototypeOf(r2));

            const r3 = Reflect.construct(C, ["c"]);
            assert.areEqual(Target, observedNewTarget, "Reflect.construct(C, args) sets new.target = Target");
            assert.areEqual(Target.prototype, Object.getPrototypeOf(r3));

            const boundDV = DataView.bind(null, new ArrayBuffer(0), 10);
            assert.throws(() => {
                Reflect.construct(boundDV, [], throwingNewTarget);
            }, RangeError, "bound DataView defers newTarget.prototype until DataView validation");

            const boundDerived = DerivedNoSuper.bind(null);
            assert.isTrue(Reflect.construct(boundDerived, [], throwingNewTarget).ok, "bound derived constructor skips newTarget.prototype");

            // Cross-realm OrdinaryCreateFromConstructor with bound functions.
            const other = WScript.LoadScript("function OtherCtor() {} OtherCtor.prototype = 1;", "samethread");
            const boundOther = other.OtherCtor.bind(null);
            const crossObj = Reflect.construct(function () { }.bind(null), [], boundOther);
            assert.areEqual(other.Object.prototype, Object.getPrototypeOf(crossObj), "cross-realm bound newTarget fallback uses target realm Object.prototype");

            // Proxy target without construct trap keeps newTarget.prototype semantics.
            const P = new Proxy(function () { }, {});
            const proxyRes = Reflect.construct(P, [], Array);
            assert.areEqual(Array.prototype, Object.getPrototypeOf(proxyRes), "Proxy without construct trap uses newTarget.prototype");

            // Proxy wrapping DataView / class / nested Proxy defers newTarget.prototype.
            const dvProxyNoTrap = new Proxy(DataView, {});
            assert.throws(() => {
                Reflect.construct(dvProxyNoTrap, [new ArrayBuffer(0), 10], throwingNewTarget);
            }, RangeError, "Proxy(DataView, {}) validates byteOffset before newTarget.prototype");

            const dvProxyRes = Reflect.construct(dvProxyNoTrap, [new ArrayBuffer(8)], Array);
            assert.isTrue(dvProxyRes instanceof Array, "Proxy(DataView, {}) uses newTarget.prototype");
            assert.areEqual(8, Object.getOwnPropertyDescriptor(DataView.prototype, "byteLength").get.call(dvProxyRes), "Proxy(DataView, {}) constructs valid DataView");

            const dvNestedProxy = new Proxy(dvProxyNoTrap, {});
            assert.throws(() => {
                Reflect.construct(dvNestedProxy, [new ArrayBuffer(0), 10], throwingNewTarget);
            }, RangeError, "nested Proxy(Proxy(DataView, {}), {}) defers newTarget.prototype");

            const derivedProxy = new Proxy(DerivedNoSuper, {});
            assert.isTrue(Reflect.construct(derivedProxy, [], throwingNewTarget).ok, "Proxy(DerivedNoSuper, {}) skips newTarget.prototype");

            let trapSawNewTarget;
            const trappedOrdinary = new Proxy(Ordinary, {
                construct: function (t, a, nt) {
                    trapSawNewTarget = nt;
                    return { ok: true };
                }
            });
            assert.isTrue(Reflect.construct(trappedOrdinary, [], throwingNewTarget).ok, "Proxy with construct trap does not touch newTarget.prototype");
            assert.areEqual(throwingNewTarget, trapSawNewTarget, "construct trap receives original newTarget");

            const proxiedBound = new Proxy(Target.bind(null, "p"), {});
            const proxiedBoundRes = Reflect.construct(proxiedBound, ["q", "r"], Target);
            assert.areEqual(Target.prototype, Object.getPrototypeOf(proxiedBoundRes), "Proxy(boundFn, {}) sets Target.prototype");
            assert.areEqual(["p", "q", "r"], observedArgs, "Proxy(boundFn, {}) preserves bound + forwarded args");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
