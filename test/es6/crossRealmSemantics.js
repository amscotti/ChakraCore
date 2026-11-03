//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Focused regression tests for the B2 cross-realm lane (doc/e3-cross-realm.md):
//  - ES2025 9.4.13 GetPrototypeFromConstructor step 4b: default [[Prototype]]
//    from GetFunctionRealm(newTarget)'s realm (proxy/bound target chains).
//  - ES2025 10.5.12/10.5.13: proxy [[Call]]/[[Construct]] validation TypeErrors
//    and the trap arguments array belong to the CALLER's realm.
//  - Revoked proxies stay constructable (ProxyCreate fixes [[Construct]] at
//    creation) and surface the revoked TypeError from [[Construct]].
//  - ES2025 10.4.2.2 ArraySetLength RangeError in the caller's realm.
//  - ES2025 7.2.2 IsArray on a revoked Proxy throws in the caller's realm
//    (JSON.stringify replacer path).
// A realm is a real second script context: WScript.LoadScript(s, 'samethread').

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var other = WScript.LoadScript('', 'samethread');

var tests = [
    {
        name: "GetPrototypeFromConstructor realm fallback: builtins via cross-realm newTarget",
        body() {
            function C() {}
            var ctors = [WeakMap, WeakSet, Map, Set, Number, Boolean, Date, Promise];
            ctors.forEach(function (ctor) {
                var args = ctor === Promise ? [function () {}] : [];
                var c = new other.Function();
                c.prototype = null;
                var o = Reflect.construct(ctor, args, c);
                assert.isTrue(Object.getPrototypeOf(o) === other[ctor.name].prototype,
                    ctor.name + " default proto must come from the newTarget's realm");
                // newTarget.prototype as a plain object still wins
                c.prototype = C.prototype;
                var o2 = Reflect.construct(ctor, args, c);
                assert.isTrue(Object.getPrototypeOf(o2) === C.prototype, ctor.name + " explicit proto");
            });
        }
    },
    {
        name: "GetPrototypeFromConstructor realm fallback: TypedArray, ArrayBuffer, Array, errors",
        body() {
            var c = new other.Function();
            c.prototype = null;
            var ta = Reflect.construct(Uint8Array, [4], c);
            assert.isTrue(Object.getPrototypeOf(ta) === other.Uint8Array.prototype, "Uint8Array");
            var ab = Reflect.construct(ArrayBuffer, [8], c);
            assert.isTrue(Object.getPrototypeOf(ab) === other.ArrayBuffer.prototype, "ArrayBuffer");
            var arr = Reflect.construct(Array, [1], c);
            assert.isTrue(Object.getPrototypeOf(arr) === other.Array.prototype, "Array");
            var e = Reflect.construct(TypeError, [], c);
            assert.isTrue(Object.getPrototypeOf(e) === other.TypeError.prototype, "TypeError");
            var ae = Reflect.construct(AggregateError, [[], 'x'], c);
            assert.isTrue(Object.getPrototypeOf(ae) === other.AggregateError.prototype, "AggregateError");
        }
    },
    {
        name: "GetFunctionRealm follows bound and proxy target chains",
        body() {
            // bound chain: bound of other-realm Function -> other's Date.prototype
            var bound = other.Function.bind(null);
            bound.prototype = undefined;
            var d = Reflect.construct(Date, [], bound);
            assert.isTrue(Object.getPrototypeOf(d) === other.Date.prototype, "bound newTarget realm");

            // proxy chain: realm2.Proxy(realm1.Function) -> realm1's Array.prototype
            var c = new other.Function();
            c.prototype = false;
            var p = new Proxy(c, {});
            var a = Reflect.construct(Array, [], p);
            assert.isTrue(Object.getPrototypeOf(a) === other.Array.prototype, "proxy newTarget realm");
        }
    },
    {
        name: "Proxy [[Call]]/[[Construct]] TypeErrors and arguments array use the caller's realm",
        body() {
            // NOTE: the handler closures stay in the main realm on purpose:
            // the caller-realm arguments array is spec-mandated to cross into
            // the trap's frame unmarshaled, which the debug build's
            // ValidateRegValue marshal check flags when the trap itself is in
            // the other realm (recorded residual in doc/e3-cross-realm.md).
            // The proxies themselves are allocated in the other realm, so the
            // realm selection under test is unchanged.
            var f = new other.Proxy(function () {}, { apply: function (_, __, args) { return args; } });
            var args = f(1, 2);
            assert.isTrue(args.constructor === Array, "apply trap args array from caller realm");
            assert.isTrue(Array.isArray(args) && args[0] === 1 && args[1] === 2, "apply trap args values");

            var g2 = new other.Proxy(function () {}, { construct: function (t, args) { return args; } });
            var cargs = new g2(3, 4);
            assert.isTrue(cargs.constructor === Array, "construct trap args array from caller realm");
            assert.isTrue(cargs[0] === 3 && cargs[1] === 4, "construct trap args values");

            // revoked proxy: [[Construct]] exists and throws caller-realm TypeError
            var rev = other.eval('(function(){ var r = Proxy.revocable(function(){}, {}); r.revoke(); return r; })()');
            var caught = null;
            try { new rev.proxy(); } catch (e) { caught = e; }
            assert.isTrue(caught !== null, "new revoked proxy throws");
            assert.isTrue(caught.constructor === TypeError, "revoked proxy construct TypeError from caller realm");
            // and via [[Call]]
            caught = null;
            try { rev.proxy(); } catch (e) { caught = e; }
            assert.isTrue(caught !== null && caught.constructor === TypeError, "revoked proxy call TypeError from caller realm");

            // trap returns a primitive from [[Construct]]: caller-realm TypeError
            var bad = new other.Proxy(function () {}, { construct: function () { return 5; } });
            caught = null;
            try { new bad(); } catch (e) { caught = e; }
            assert.isTrue(caught !== null && caught.constructor === TypeError, "construct trap non-object TypeError from caller realm");
        }
    },
    {
        name: "ArraySetLength RangeError is thrown in the caller's realm",
        body() {
            var arr = new other.Array();
            var caught = null;
            try { arr.length = 4294967296; } catch (e) { caught = e; }
            assert.isTrue(caught !== null, "array.length overflow throws");
            assert.isTrue(caught.constructor === RangeError, "RangeError from caller realm");
        }
    },
    {
        name: "IsArray on a revoked Proxy throws in the caller's realm (JSON replacer)",
        body() {
            var r = other.eval('(function(){ var rev = Proxy.revocable([], {}); rev.revoke(); return rev; })()');
            var caught = null;
            try { JSON.stringify({}, r.proxy); } catch (e) { caught = e; }
            assert.isTrue(caught !== null, "JSON.stringify with revoked replacer throws");
            assert.isTrue(caught.constructor === TypeError, "TypeError from caller realm");
        }
    },
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
