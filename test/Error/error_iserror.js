//-------------------------------------------------------------------------------------------------------
// Copyright (c) 2026 ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "Error.isError property descriptor, name, length, and non-constructibility",
        body: function () {
            var desc = Object.getOwnPropertyDescriptor(Error, "isError");
            assert.areEqual("function", typeof Error.isError);
            assert.isTrue(desc.writable, "Error.isError should be writable");
            assert.isFalse(desc.enumerable, "Error.isError should not be enumerable");
            assert.isTrue(desc.configurable, "Error.isError should be configurable");

            assert.areEqual("isError", Error.isError.name);
            assert.areEqual(1, Error.isError.length);

            assert.throws(function () { new Error.isError(); }, TypeError);
            assert.throws(function () { Reflect.construct(Error.isError, []); }, TypeError);
        }
    },
    {
        name: "Error.isError returns true for built-in and engine-thrown Error instances",
        body: function () {
            assert.isTrue(Error.isError(new Error()));
            assert.isTrue(Error.isError(Error()));
            assert.isTrue(Error.isError(new EvalError()));
            assert.isTrue(Error.isError(new RangeError()));
            assert.isTrue(Error.isError(new ReferenceError()));
            assert.isTrue(Error.isError(new SyntaxError()));
            assert.isTrue(Error.isError(new TypeError()));
            assert.isTrue(Error.isError(TypeError("msg")));
            assert.isTrue(Error.isError(new URIError()));
            assert.isTrue(Error.isError(new AggregateError([])));

            if (typeof WebAssembly !== "undefined") {
                assert.isTrue(Error.isError(new WebAssembly.CompileError()));
                assert.isTrue(Error.isError(new WebAssembly.RuntimeError()));
                assert.isTrue(Error.isError(new WebAssembly.LinkError()));
            }

            try {
                null.x;
            } catch (e) {
                assert.isTrue(Error.isError(e));
            }

            try {
                eval("{{{");
            } catch (e) {
                assert.isTrue(Error.isError(e));
            }
        }
    },
    {
        name: "Error.isError returns true for Error subclasses and Reflect.construct with Error",
        body: function () {
            class SubError extends Error {}
            class SubTypeError extends TypeError {}
            class SubAggregateError extends AggregateError {}

            assert.isTrue(Error.isError(new SubError()));
            assert.isTrue(Error.isError(new SubTypeError()));
            assert.isTrue(Error.isError(new SubAggregateError([])));

            function CustomTarget() {}
            var constructed = Reflect.construct(Error, ["abc"], CustomTarget);
            assert.isTrue(Error.isError(constructed));
            assert.areEqual(CustomTarget.prototype, Object.getPrototypeOf(constructed));
        }
    },
    {
        name: "Error.isError returns false for Error prototypes and Error constructors",
        body: function () {
            assert.isFalse(Error.isError(Error.prototype));
            assert.isFalse(Error.isError(EvalError.prototype));
            assert.isFalse(Error.isError(RangeError.prototype));
            assert.isFalse(Error.isError(ReferenceError.prototype));
            assert.isFalse(Error.isError(SyntaxError.prototype));
            assert.isFalse(Error.isError(TypeError.prototype));
            assert.isFalse(Error.isError(URIError.prototype));
            assert.isFalse(Error.isError(AggregateError.prototype));

            assert.isFalse(Error.isError(Error));
            assert.isFalse(Error.isError(EvalError));
            assert.isFalse(Error.isError(RangeError));
            assert.isFalse(Error.isError(ReferenceError));
            assert.isFalse(Error.isError(SyntaxError));
            assert.isFalse(Error.isError(TypeError));
            assert.isFalse(Error.isError(URIError));
            assert.isFalse(Error.isError(AggregateError));

            if (typeof WebAssembly !== "undefined") {
                assert.isFalse(Error.isError(WebAssembly.CompileError.prototype));
                assert.isFalse(Error.isError(WebAssembly.RuntimeError.prototype));
                assert.isFalse(Error.isError(WebAssembly.LinkError.prototype));
                assert.isFalse(Error.isError(WebAssembly.CompileError));
            }
        }
    },
    {
        name: "Error.isError returns false for Proxies and objects inheriting from Error",
        body: function () {
            var realErr = new Error("real");
            assert.isFalse(Error.isError(new Proxy(realErr, {})));
            assert.isFalse(Error.isError(new Proxy(Error.prototype, {})));
            var rev = Proxy.revocable(realErr, {});
            assert.isFalse(Error.isError(rev.proxy));
            rev.revoke();
            assert.isFalse(Error.isError(rev.proxy));

            assert.isFalse(Error.isError(Object.create(Error.prototype)));
            assert.isFalse(Error.isError(Object.create(realErr)));

            var fake = {
                __proto__: Error.prototype,
                constructor: Error,
                message: "fake",
                stack: realErr.stack,
                [Symbol.toStringTag]: "Error"
            };
            assert.isFalse(Error.isError(fake));

            function FakeCtor() {}
            assert.isFalse(Error.isError(Reflect.construct(FakeCtor, [], Error)));
        }
    },
    {
        name: "Error.isError returns false for primitives, Symbols, BigInts, and plain objects",
        body: function () {
            assert.isFalse(Error.isError());
            assert.isFalse(Error.isError(undefined));
            assert.isFalse(Error.isError(null));
            assert.isFalse(Error.isError(true));
            assert.isFalse(Error.isError(false));
            assert.isFalse(Error.isError(0));
            assert.isFalse(Error.isError(-0));
            assert.isFalse(Error.isError(42));
            assert.isFalse(Error.isError(NaN));
            assert.isFalse(Error.isError(Infinity));
            assert.isFalse(Error.isError(-Infinity));
            assert.isFalse(Error.isError(""));
            assert.isFalse(Error.isError("Error"));
            assert.isFalse(Error.isError(Symbol("err")));
            assert.isFalse(Error.isError(0n));
            assert.isFalse(Error.isError(42n));
            assert.isFalse(Error.isError({}));
            assert.isFalse(Error.isError([]));
            assert.isFalse(Error.isError(function () {}));
            assert.isFalse(Error.isError(/err/));
            assert.isFalse(Error.isError(new Date()));
            assert.isFalse(Error.isError(new Number(1)));

            assert.isTrue(Error.isError.call(null, new Error()));
            assert.isFalse(Error.isError.call(42, {}));
        }
    },
    {
        name: "Error.isError handles cross-realm Errors and non-Errors",
        body: function () {
            var other = WScript.LoadScript("", "samethread");

            assert.isTrue(Error.isError(new other.Error("cross")));
            assert.isTrue(Error.isError(other.Error("cross")));
            assert.isTrue(Error.isError(new other.TypeError("cross")));
            assert.isTrue(Error.isError(new other.RangeError("cross")));
            assert.isTrue(Error.isError(new other.AggregateError([])));

            assert.isFalse(Error.isError(other.Error));
            assert.isFalse(Error.isError(other.Error.prototype));
            assert.isFalse(Error.isError(other.TypeError.prototype));
            assert.isFalse(Error.isError(new other.Object()));
            assert.isFalse(Error.isError(new other.Proxy(new other.Error(), {})));
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
