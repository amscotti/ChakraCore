//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// Focused regressions for the B5 class-tail lane (doc/e6-class-tail.md):
//
// 1. Legacy `caller`/`arguments` are no longer virtual own properties of
//    sloppy script functions: hasOwnProperty is false, defineProperty can add
//    real own properties, and those are readable from strict code (ES2025
//    sec-addrestrictedfunctionproperties installs the poison accessors on
//    %FunctionPrototype% only). The legacy stack-walk read for sloppy
//    functions without an own property is preserved.
// 2. Derived-constructor return completions are processed at function exit
//    ([[Construct]] steps 9-11), after finally blocks and iterator close
//    handlers: `super()` in a finally satisfies a `return` in the try, the
//    TypeError for a primitive return value is not catchable by the
//    constructor body, and an iterator return handler still runs first.
// 3. `super[expr]` checks the this-binding before evaluating expr and
//    captures GetSuperBase before the key's ToPropertyKey; `super(args)`
//    evaluates arguments before the IsConstructor TypeError.
// 4. Class constructors own their properties in spec order length, name,
//    prototype; anonymous class expressions own name === "".
// 5. Class declarations produce an empty completion value (eval result).
// 6. `yield` followed by a newline then `*` is a SyntaxError.
// 7. The default derived constructor forwards its argument list without the
//    iterator protocol.
// 8. `await` as a label inside a class static block is a SyntaxError, while a
//    nested (non-async) function named `await` (and `await` formals) is legal.

function scriptOK(name, source) {
    var passed = false;
    try {
        WScript.LoadScript(source, "samethread");
        passed = true;
    } catch (e) {
        passed = false;
    }
    assert.isTrue(passed, name + " should parse and run");
}

function scriptThrowsSyntaxError(name, source) {
    var passed = false;
    try {
        WScript.LoadScript(source, "samethread");
    } catch (e) {
        passed = e.constructor.name === "SyntaxError";
    }
    assert.isTrue(passed, name + " should be a SyntaxError");
}

var tests = [
    {
        name: "sloppy functions have no own caller/arguments; own properties are definable and readable",
        body() {
            function sloppy() { return 1; }

            assert.isFalse(sloppy.hasOwnProperty("caller"), "no own caller");
            assert.isFalse(sloppy.hasOwnProperty("arguments"), "no own arguments");
            assert.areEqual(undefined, Object.getOwnPropertyDescriptor(sloppy, "caller"),
                "gOPD of caller is undefined");
            var ownNames = Object.getOwnPropertyNames(sloppy);
            assert.areEqual(-1, ownNames.indexOf("caller"), "own keys exclude caller");
            assert.areEqual(-1, ownNames.indexOf("arguments"), "own keys exclude arguments");

            // Defining real own properties works and strict code can read them.
            var withGetter = function () { return 2; };
            Object.defineProperty(withGetter, "caller", { get: function () { return "OWNGET"; }, configurable: true });
            var withValue = function () { return 3; };
            Object.defineProperty(withValue, "caller", { value: "OWNVAL", writable: true, configurable: true });
            var readOwn = new Function("f", "g", "'use strict'; return f.caller + '/' + g.caller;");
            assert.areEqual("OWNGET/OWNVAL", readOwn(withGetter, withValue), "own caller readable from strict code");

            // Legacy read for a sloppy function without an own property still
            // works: callee.caller reaches the sloppy caller (the 10.6-13-a-2
            // shape). Note: when the caller is a strict function this engine
            // has a pre-existing, parse-mode-dependent split between
            // returning null and throwing the restricted-property TypeError
            // (ES2025 removed the legacy accessor; V8 returns null); that
            // residual is carded in doc/e6-class-tail.md.
            var legacy = new Function("\n" +
                "var r;\n" +
                "(function sloppyOuter() { r = (function inner() { return inner.caller; })(); })();\n" +
                "return r !== undefined && r !== null && r.name === 'sloppyOuter';\n");
            assert.isTrue(legacy(), "legacy caller read returns the sloppy caller");

            // Strict functions still throw on .caller through %FunctionPrototype%.
            assert.throws(function () { return (function () { 'use strict'; }).caller; }, TypeError,
                "strict function caller read throws");
        }
    },
    {
        name: "derived constructor return checks run at function exit",
        body() {
            // super() in a finally satisfies a `return` in the try/catch.
            var o1 = new (class extends Object {
                constructor() {
                    try { throw null; } catch (e) { return; } finally { super(); }
                }
            })();
            assert.areEqual("object", typeof o1, "finally super() satisfies the return");

            var o2 = new (class extends Object {
                constructor() {
                    try { return; } finally { super(); }
                }
            })();
            assert.areEqual("object", typeof o2, "try return with finally super()");

            // The TypeError for `return 0` is not catchable inside the body.
            assert.throws(function () {
                new (class extends Object {
                    constructor() { try { return 0; } catch (e) { return; } }
                })();
            }, TypeError, "return 0 TypeError escapes the catch");

            assert.throws(function () {
                new (class extends Object {
                    constructor() { super(); try { return 0; } catch (e) { super(); } }
                })();
            }, TypeError, "return 0 TypeError escapes the catch after super");

            // An iterator return handler can still call super() before the
            // missing-super ReferenceError is raised at exit (arrow variant of
            // the corpus shape: the return handler calls super() through the
            // closure created in the constructor).
            var iter = {
                [Symbol.iterator]() { return this; },
                next() { return { done: false }; },
                return() { iter.f(); return { done: true }; }
            };
            var o3 = new (class extends Object {
                constructor() {
                    iter.f = function () { };
                    iter.f = () => super();
                    for (var k of iter) { return; }
                }
            })();
            assert.areEqual("object", typeof o3, "iterator close can call super before the exit check");

            // An iterator return handler that throws wins over the pending
            // primitive-return TypeError (raised after the body completes).
            var marker = new Error("iter-close");
            var iter2 = {
                [Symbol.iterator]() { return this; },
                next() { return { done: false }; },
                return() { throw marker; }
            };
            var thrown = null;
            try {
                new (class extends Object {
                    constructor() { super(); for (var k of iter2) { return 0; } }
                })();
            } catch (e) { thrown = e; }
            assert.areEqual(marker, thrown, "iterator close error takes precedence over return 0");

            // Returning an object skips the this-binding check entirely.
            var replaced = { tag: 1 };
            var o4 = new (class extends Object {
                constructor() { return replaced; }
            })();
            assert.areEqual(replaced, o4, "object return replaces this without a super call");
        }
    },
    {
        name: "super property evaluation order: this-binding before the key, base captured before ToPropertyKey",
        body() {
            // this-binding check precedes key evaluation.
            var keyRan = false;
            assert.throws(function () {
                new (class extends Object {
                    constructor() { return super[keyRan = true]; }
                })();
            }, ReferenceError, "uninitialized this check fires first");
            assert.isFalse(keyRan, "the key expression did not run");

            // GetSuperBase is captured before the key's ToPropertyKey.
            var proto = { p: 1 };
            var proto2 = { p: -1 };
            var obj = {
                __proto__: proto,
                m() { return super[key] += 1; }
            };
            var key = {
                toString() {
                    Object.setPrototypeOf(obj, proto2);
                    return "p";
                }
            };
            assert.areEqual(2, obj.m(), "compound super store uses the captured base");
            assert.areEqual(2, proto.p, "store went through the captured base's property");

            // The plain store form also keeps the captured base.
            var protoB = { q: 10 };
            var objB = {
                __proto__: protoB,
                set(v) { super[otherKey] = v; }
            };
            var otherKey = {
                toString() {
                    Object.setPrototypeOf(objB, proto2);
                    return "q";
                }
            };
            objB.set(7);
            assert.areEqual(7, protoB.q, "plain super store uses the captured base");

            // super() evaluates arguments before the not-a-constructor TypeError.
            var argEvaluated = false;
            var caught = null;
            var C = class extends Object {
                constructor() {
                    try { super(argEvaluated = true); } catch (e) { caught = e; }
                }
            };
            Object.setPrototypeOf(C, parseInt);
            try { new C(); } catch (e) { }
            assert.areEqual(true, caught instanceof TypeError, "super() with non-constructor proto throws TypeError");
            assert.isTrue(argEvaluated, "the argument expression ran first");
        }
    },
    {
        name: "class constructor own-property order and anonymous class name",
        body() {
            assert.areEqual("length,name,prototype", Object.getOwnPropertyNames(class { }).join(","),
                "anonymous class ctor own keys");
            assert.areEqual("length,name,prototype,method",
                Object.getOwnPropertyNames(class { static method() { } }).join(","),
                "static members follow prototype");
            assert.areEqual("length,name,prototype",
                Object.getOwnPropertyNames(class Named { }).join(","), "named class ctor own keys");

            var anon = Object.getOwnPropertyDescriptor(class { }, "name");
            assert.areEqual("", anon.value, "anonymous class expression name value");
            assert.isFalse(anon.writable, "name is not writable");
            assert.isFalse(anon.enumerable, "name is not enumerable");
            assert.isTrue(anon.configurable, "name is configurable");

            var named = Object.getOwnPropertyDescriptor(class cls { }, "name");
            assert.areEqual("cls", named.value, "named class expression name value");

            // NamedEvaluation still renames in place.
            var assigned = class { };
            assert.areEqual("assigned", assigned.name, "assignment inference still applies");
        }
    },
    {
        name: "class declarations produce an empty completion value",
        body() {
            assert.areEqual(undefined, eval("class Cptn1 { }"), "lone class declaration");
            assert.areEqual(1, eval("1; class Cptn2 { }"), "class declaration keeps the previous value");
            assert.areEqual(2, eval("2; class Cptn3 { }; class Cptn4 { }"), "trailing declarations keep the value");
            var expr = eval("(class Cptn5 { })");
            assert.areEqual("Cptn5", expr.name, "class expressions still evaluate to the constructor");
        }
    },
    {
        name: "yield followed by a newline and * is a SyntaxError",
        body() {
            scriptThrowsSyntaxError("generator declaration yield newline star",
                "function* g() { yield\n* 1\n}");
            scriptThrowsSyntaxError("class generator method yield newline star",
                "class A { *g() { yield\n* 1\n} }");
            scriptThrowsSyntaxError("object generator method yield newline star",
                "var o = { *g() { yield\n* 1\n} };");
            scriptOK("yield newline operand is still valid",
                "function* g() { yield\n1\n}");
            scriptOK("yield star on the same line is still valid",
                "function* g() { yield * [1]; }");
        }
    },
    {
        name: "the default derived constructor does not use the iterator protocol",
        body() {
            var iteratorInvoked = false;
            var savedIterator = Array.prototype[Symbol.iterator];
            Array.prototype[Symbol.iterator] = function () {
                iteratorInvoked = true;
                throw new Error("@@iterator invoked");
            };
            try {
                class Base { constructor(value) { this.value = value; } }
                class Derived extends Base { }
                var instance = new Derived(5);
                assert.areEqual(5, instance.value, "args forwarded to the base constructor");
                assert.isFalse(iteratorInvoked, "@@iterator was not consulted");

                // Multiple arguments forward in order.
                class Base2 { constructor(...rest) { this.rest = rest; } }
                class Derived2 extends Base2 { }
                var instance2 = new Derived2(1, 2, 3);
                assert.areEqual("1,2,3", instance2.rest.join(","), "all arguments forwarded");
                assert.isFalse(iteratorInvoked, "@@iterator still not consulted");
            } finally {
                Array.prototype[Symbol.iterator] = savedIterator;
            }

            // User-written spread keeps the observable iteration.
            var userIterated = false;
            Array.prototype[Symbol.iterator] = function () {
                userIterated = true;
                return savedIterator.call(this);
            };
            try {
                class Base3 { constructor(...rest) { this.rest = rest; } }
                class Derived3 extends Base3 { constructor(...args) { super(...args); } }
                new Derived3(9);
                assert.isTrue(userIterated, "explicit super(...args) spread still iterates");
            } finally {
                Array.prototype[Symbol.iterator] = savedIterator;
            }
        }
    },
    {
        name: "await in class static blocks: labels rejected, nested function names allowed",
        body() {
            scriptThrowsSyntaxError("label named await in static block",
                "class C { static { await: 0; } }");
            scriptThrowsSyntaxError("label named await in nested block",
                "class C { static { { await: 0; } } }");
            scriptThrowsSyntaxError("await expression in static block",
                "class C { static { await; } }");
            scriptOK("function named await in static block",
                "class C { static { (function await(await) {}); } }");
            scriptOK("generator named await in static block",
                "class C { static { (function* await(await) {}); } }");
            scriptOK("label named await in a nested function",
                "class C { static { (function() { await: 0; }); } }");
            scriptOK("ordinary labels in static block",
                "class C { static { outer: inner: 0; } }");
        }
    },
    {
        name: "super element references capture GetSuperBase after the key Expression (MakeSuperPropertyReference order)",
        body() {
            function events(kind, swapInExpression) {
                var log = [];
                var oldP = { get g() { log.push("old-get"); return "old"; }, set g(v) { log.push("old-set:" + v); } };
                var newP = { get g() { log.push("new-get"); return "new"; }, set g(v) { log.push("new-set:" + v); } };
                var obj;
                obj = {
                    __proto__: oldP,
                    m(swap) {
                        // Expression-time swap: the comma expression runs while
                        // the key Expression is evaluated, i.e. BEFORE
                        // MakeSuperPropertyReference performs GetSuperBase
                        // (sec-super-keyword-runtime-semantics-evaluation), so
                        // both the load and the store must use the NEW chain.
                        if (swapInExpression) {
                            if (kind === "store") { super[(swap(), "g")] = 9; return "stored"; }
                            if (kind === "compound") { return super[(swap(), "g")] += 1; }
                            if (kind === "logical") { return super[(swap(), "g")] ||= 5; }
                            if (kind === "inc") { return super[(swap(), "g")]++; }
                            return super[(swap(), "g")];
                        }
                        // ToPropertyKey-time swap control: the toString runs
                        // AFTER GetSuperBase, so the captured base stays the
                        // OLD chain (test262
                        // prop-expr-getsuperbase-before-topropertykey-*).
                        var key = { toString() { swap(); return "g"; } };
                        if (kind === "store") { super[key] = 9; return "stored"; }
                        if (kind === "compound") { return super[key] += 1; }
                        return super[key];
                    }
                };
                var swap = function () { Object.setPrototypeOf(obj, newP); };
                var r = obj.m(swap);
                return r + " | " + log.join(",");
            }

            // Expression-time swap: every form resolves the new chain.
            assert.areEqual("new | new-get", events("read", true), "expression swap read");
            assert.areEqual("stored | new-set:9", events("store", true), "expression swap store");
            assert.areEqual("new1 | new-get,new-set:new1", events("compound", true), "expression swap compound");
            assert.areEqual("new | new-get", events("logical", true), "expression swap logical (falsy: no store)");
            assert.areEqual("NaN | new-get,new-set:NaN", events("inc", true), "expression swap increment");

            // ToPropertyKey-time swap control: the captured base is the old
            // chain for the load and the store (spec keeps one reference; the
            // Node oracle re-resolves the compound store through the new
            // chain -- a V8 deviation from the single-reference PutValue, not
            // followed here, matching the B4-2 card's store-through-the-second-
            // key lineage).
            assert.areEqual("old | old-get", events("read", false), "toString swap read uses the old chain");
            assert.areEqual("stored | old-set:9", events("store", false), "toString swap store uses the old chain");
            assert.areEqual("old1 | old-get,old-set:old1", events("compound", false), "toString swap compound keeps the captured base");
        }
    },
    {
        name: "own writable caller/arguments properties win over the legacy read-only layer",
        body() {
            var f = function () { return 1; };
            Object.defineProperty(f, "caller", { value: 42, writable: true, configurable: true });
            assert.areEqual(42, f.caller, "own caller readable");
            f.caller = 7;
            assert.areEqual(7, f.caller, "plain assignment writes the own caller");
            assert.isTrue(Reflect.set(f, "caller", 9), "Reflect.set returns true for the own caller");
            assert.areEqual(9, f.caller, "Reflect.set wrote the own caller");

            var g = function () { return 2; };
            Object.defineProperty(g, "arguments", { value: 40, writable: true, configurable: true });
            g.arguments = 8;
            assert.areEqual(8, g.arguments, "plain assignment writes the own arguments");

            // Without an own property the legacy layer is unchanged: sloppy
            // writes stay silent no-ops and strict writes throw.
            var h = function () { return 3; };
            h.caller = 1;
            assert.areEqual(null, h.caller, "legacy read still works after a sloppy write");
            assert.throws(function () { "use strict"; h.caller = 1; }, TypeError,
                "strict write to the legacy caller still throws");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
