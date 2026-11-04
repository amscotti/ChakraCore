//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// B8: element-level toLocaleString dispatch for Array.prototype.toLocaleString
// and %TypedArray%.prototype.toLocaleString. Each element contributes
// ToString(Invoke(element, "toLocaleString")), so the formatters on
// Number/Boolean/Symbol/BigInt.prototype are user-observable (overrides,
// coercion of the returned value, abrupt throws, receiver boxing). See
// doc/e9-bigint-partial.md.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var savedNumber = Number.prototype.toLocaleString;
var savedSymbol = Symbol.prototype.toLocaleString;
var savedBigInt = BigInt.prototype.toLocaleString;

function withOverrides(overrides, action) {
    Number.prototype.toLocaleString = overrides.number || savedNumber;
    Symbol.prototype.toLocaleString = overrides.symbol || savedSymbol;
    BigInt.prototype.toLocaleString = overrides.bigint || savedBigInt;
    try {
        action();
    } finally {
        Number.prototype.toLocaleString = savedNumber;
        Symbol.prototype.toLocaleString = savedSymbol;
        BigInt.prototype.toLocaleString = savedBigInt;
    }
}

var tests = [
    {
        name: "Array elements invoke their own toLocaleString",
        body: function () {
            withOverrides({ number: function () { return "N"; } }, function () {
                assert.strictEqual("N,N", [1, 2].toLocaleString(),
                    "a Number.prototype.toLocaleString override is observed through arrays");
            });
            withOverrides({}, function () {
                delete Number.prototype.toLocaleString;
                try {
                    assert.strictEqual("1", [1].toLocaleString(),
                        "a deleted method falls back to Object.prototype.toLocaleString (ToString)");
                } finally {
                    Number.prototype.toLocaleString = savedNumber;
                }
                assert.strictEqual("1", [1].toLocaleString(), "the restored default formats the value");
            });
        }
    },
    {
        name: "TypedArray elements invoke their own toLocaleString (Number and BigInt)",
        body: function () {
            withOverrides({ number: function () { return "n"; } }, function () {
                assert.strictEqual("n,n", new Float64Array([42, 0]).toLocaleString(),
                    "Float64Array elements dispatch to the overridden Number.prototype.toLocaleString");
            });
            withOverrides({ bigint: function () { return "b"; } }, function () {
                assert.strictEqual("b,b", new BigInt64Array([42n, 0n]).toLocaleString(),
                    "BigInt64Array elements dispatch to the overridden BigInt.prototype.toLocaleString");
            });
            withOverrides({}, function () {
                assert.strictEqual("42,0", new BigInt64Array([42n, 0n]).toLocaleString(),
                    "the default BigInt formatter still produces the decimal digits");
            });
        }
    },
    {
        name: "Returned values are coerced with ToString and throws propagate",
        body: function () {
            withOverrides({ number: function () { return { toString: undefined, valueOf: function () { return "V"; } }; } }, function () {
                assert.strictEqual("V", [5].toLocaleString(), "valueOf coerces a returned object");
            });
            withOverrides({ number: function () { return { toString: function () { return "T"; } }; } }, function () {
                assert.strictEqual("T", [5].toLocaleString(), "toString coerces a returned object");
            });
            withOverrides({ number: function () { return { toString: undefined, valueOf: function () { return {}; } }; } }, function () {
                assert.throws(function () { [5].toLocaleString(); }, TypeError,
                    "an uncoercible return value throws TypeError from ToString");
            });
            withOverrides({ number: function () { throw new RangeError("boom"); } }, function () {
                assert.throws(function () { [1, 2].toLocaleString(); }, RangeError,
                    "an abrupt element toLocaleString propagates");
            });
            withOverrides({ bigint: function () { throw new RangeError("bigint-boom"); } }, function () {
                assert.throws(function () { new BigInt64Array([1n]).toLocaleString(); }, RangeError,
                    "an abrupt BigInt element toLocaleString propagates");
            });
        }
    },
    {
        name: "undefined and null elements contribute nothing; non-callable methods throw",
        body: function () {
            assert.strictEqual(",", [undefined, null].toLocaleString(),
                "undefined/null elements contribute the empty string");
            withOverrides({ number: 42 }, function () {
                assert.throws(function () { [1].toLocaleString(); }, TypeError,
                    "a non-callable toLocaleString property throws");
            });
            withOverrides({ symbol: 42 }, function () {
                assert.throws(function () { [Symbol("x")].toLocaleString(); }, TypeError,
                    "symbol elements resolve the method through Symbol.prototype too");
            });
        }
    },
    {
        name: "Symbol elements use Symbol.prototype.toLocaleString",
        body: function () {
            var s = Symbol("tagged");
            withOverrides({ symbol: function () { return "S"; } }, function () {
                assert.strictEqual("S", [s].toLocaleString(), "the symbol override is observed");
            });
            withOverrides({}, function () {
                assert.strictEqual("Symbol(tagged)", [s].toLocaleString(),
                    "the default formatter is ToString of the symbol");
            });
        }
    },
    {
        name: "Element receiver boxing follows Call semantics",
        body: function () {
            withOverrides({ number: function () { return typeof this; } }, function () {
                assert.strictEqual("object", [7].toLocaleString(),
                    "a sloppy-mode override sees the boxed primitive");
            });
            var strict = new Function("n", '"use strict"; n.toLocaleString = function () { return typeof this; }; return [7].toLocaleString();');
            withOverrides({}, function () {
                assert.strictEqual("number", strict(Number.prototype), "a strict-mode override sees the primitive");
            });
        }
    },
    {
        name: "Element overrides observe detach and shrink during iteration",
        body: function () {
            var buffer = new ArrayBuffer(8 * 4);
            var view = new Float64Array(buffer);
            withOverrides({ number: function () { buffer.transfer(); return "x"; } }, function () {
                assert.strictEqual("x,,,", view.toLocaleString(),
                    "the captured length iterates four elements; detached reads contribute nothing");
            });
            var rab = new ArrayBuffer(8 * 4, { maxByteLength: 8 * 8 });
            var fixed = new Float64Array(rab, 0, 4);
            var remaining = 2;
            withOverrides({ number: function () {
                if (--remaining === 0) { rab.resize(8 * 2); }
                return savedNumber.call(this);
            } }, function () {
                assert.strictEqual("0,0,,", fixed.toLocaleString(),
                    "out-of-bounds reads after a shrink contribute nothing");
            });
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
