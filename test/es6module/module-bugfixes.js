//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// ES6 Module tests for bugfixes

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function testModuleScript(source, message, shouldFail = false) {
    let testfunc = () => testRunner.LoadModule(source, 'samethread', shouldFail);

    if (shouldFail) {
        let caught = false;

        // We can't use assert.throws here because the SyntaxError used to construct the thrown error
        // is from a different context so it won't be strictly equal to our SyntaxError.
        try {
            testfunc();
        } catch(e) {
            caught = true;

            // Compare toString output of SyntaxError and other context SyntaxError constructor.
            assert.areEqual(e.constructor.toString(), SyntaxError.toString(), message);
        }

        assert.isTrue(caught, `Expected error not thrown: ${message}`);
    } else {
        assert.doesNotThrow(testfunc, message);
    }
}

var tests = [
    {
        name: "OS12113549: Assertion on module export in ProcessCapturedSym",
        body: function() {
            let functionBody =
                `
                import { module1_exportbinding_0 as module2_localbinding_0 } from 'bug_OS12113549_module1.js';
                assert.areEqual({"BugID": "OS12113549"}, module2_localbinding_0);
                `
            testRunner.LoadModule(functionBody, 'samethread');
        }
    },
    {
        name: "Memory leak test on syntax error",
        body: function() {
            try {
                WScript.LoadModule('');
                WScript.LoadModule('1');
                WScript.LoadModule('const a = () -> {};');
            } catch(e) {
                // no-op
            }
        }
    },
    {
        name: "Issue 4482: Indirect circular module dependencies",
        body: function() {
            let functionBody = "import 'module_4482_dep1.js';"
            testRunner.LoadModule(functionBody);
        }
    },
    {
        name: "Issue 4570: Module that appears multiple times in dependency tree",
        body: function() {
            let functionBody = "import 'module_4570_dep1.js';"
            testRunner.LoadModule(functionBody);
        }
    },
    {
        name: "Issue 5171: Incorrect module load order",
        body: function() {
            WScript.RegisterModuleSource("obj.js", `export const obj = {a:false, b: false, c: false};`);
            WScript.RegisterModuleSource("a.js",`
                import {obj} from "obj.js";
                assert.isTrue(obj.b);
                assert.isFalse(obj.c);
                assert.isFalse(obj.a);
                obj.a = true;`);
            WScript.RegisterModuleSource("b.js",`
                import {obj} from "obj.js";
                assert.isFalse(obj.b);
                assert.isFalse(obj.c);
                assert.isFalse(obj.a);
                obj.b = true;`);
            WScript.RegisterModuleSource("c.js",`
                import {obj} from "obj.js";
                assert.isTrue(obj.b);
                assert.isFalse(obj.c);
                assert.isTrue(obj.a);
                obj.c = true;`);
            const start = 'import "b.js"; import "a.js"; import "c.js";';
            testRunner.LoadModule(start);
        }
    },
    {
        name: "Issue 6261: Specifier dependent module load order",
        body: function() {
            WScript.RegisterModuleSource("dep", `export const obj = {a:false, b: false, c: false};`);
            WScript.RegisterModuleSource("one",`
                import {obj} from "dep";
                assert.isFalse(obj.b);
                assert.isFalse(obj.c);
                assert.isFalse(obj.a);
                obj.a = true;`);
            WScript.RegisterModuleSource("two",`
                import {obj} from "dep";
                assert.isFalse(obj.b);
                assert.isFalse(obj.c);
                assert.isTrue(obj.a);
                obj.b = true;`);
            WScript.RegisterModuleSource("three",`
                import {obj} from "dep";
                assert.isTrue(obj.b);
                assert.isFalse(obj.c);
                assert.isTrue(obj.a);
                obj.c = true;`);
            const start = 'import "one"; import "two"; import "three";';
            testRunner.LoadModule(start);
        }
    },
    {
        // https://github.com/Microsoft/ChakraCore/issues/5501
        name : "Issue 5501: Indirect exports excluded from namespace object - POC 1",
        body()
        {
            WScript.RegisterModuleSource("test5501Part1", `
                export {bar as foo} from "test5501Part1";
                export const bar = 5;
                import * as stuff from "test5501Part1";
                assert.areEqual(stuff.bar, stuff.foo);
            `);
            testRunner.LoadModule("import 'test5501Part1'");
        }
    },
    {
        name : "Issue 5501: Indirect exports excluded from namespace object - POC 2",
        body()
        {
            WScript.RegisterModuleSource("test5501Part2a", "export default function () { return true; }");
            WScript.RegisterModuleSource("test5501Part2b", "export {default as aliasName} from 'test5501Part2a'");

            testRunner.LoadModule(`
                import {aliasName} from 'test5501Part2b';
                assert.isTrue(aliasName());
            `);
        }
    },
    {
        name : "Issue 6133: Child module imports non-existant export from another module",
        body()
        {
            WScript.RegisterModuleSource("test6133a", 'import Default from "test6133b";');
            WScript.RegisterModuleSource("test6133b", 'export function notDefault () {}');

            testRunner.LoadModule('import "test6133a";', undefined, true);
        }
    },
    {
        name : "Issue 5236: Module function exports not hoisted test",
        body()
        {
            WScript.RegisterModuleSource("test5236a", `
                import {default as Default, bar, foo} from "test5236b";
                export default function () {}
                export function one() {}
                export var two = function () {}

                bar();
                assert.isNotUndefined(Default);
                foo();
                `);
            WScript.RegisterModuleSource("test5236b", `
                import Default from "test5236c";

                export function bar() {}
                export default class {}
                export var foo = function () {}

                Default();
                `);
            WScript.RegisterModuleSource("test5236c", `
                import {default as Default, one, two} from "test5236a";
                import otherDefault from "test5236b";
                export default function bar() {}

                Default();
                one();
                assert.isUndefined(two);
                // otherDefault is test5236b's default-exported class,
                // still uninitialized while test5236c evaluates: reading it
                // must throw ReferenceError (Node oracle throws on the
                // identical shape), not yield undefined.
                assert.throws(() => otherDefault, ReferenceError, "class default export is TDZ until its module evaluates");
                `);

            testRunner.LoadModule('import "test5236a";', undefined, false);
        }
    },
    {
        name: "Deferred import reassignment TypeError, default binding update, anon default .name, and NamedEvaluation o[expr]",
        body()
        {
            WScript.RegisterModuleSource("deferConstTarget", `
                export var v = 1;
                export function fn() {}
                export function* gen() {}
            `);
            testRunner.LoadModule(`
                import { v, fn, gen } from "deferConstTarget";
                import * as ns from "deferConstTarget";
                assert.throws(() => { v = 2; }, TypeError, "deferred import var reassignment throws");
                assert.throws(() => { fn = null; }, TypeError, "deferred import fn reassignment throws");
                assert.throws(() => { gen = null; }, TypeError, "deferred import gen reassignment throws");
                assert.throws(() => { ns = null; }, TypeError, "deferred import namespace reassignment throws");
            `);

            WScript.RegisterModuleSource("dfltFnUpdate", `
                export default function fn() {
                    fn = 2;
                    return 1;
                }
            `);
            WScript.RegisterModuleSource("dfltClsUpdate", `
                export default class Cls {}
                Cls = 42;
            `);
            testRunner.LoadModule(`
                import fnVal from "dfltFnUpdate";
                import clsVal from "dfltClsUpdate";
                assert.areEqual(1, fnVal());
                assert.areEqual(2, fnVal, "default exported named function binding updates default export");
                assert.areEqual(42, clsVal, "default exported named class outer binding updates default export");
            `);

            WScript.RegisterModuleSource("anonFn", "export default function() {}");
            WScript.RegisterModuleSource("anonGen", "export default function*() {}");
            WScript.RegisterModuleSource("anonAsync", "export default async function() {}");
            WScript.RegisterModuleSource("anonParenFn", "export default (function() {});");
            WScript.RegisterModuleSource("anonParenGen", "export default (function*() {});");
            WScript.RegisterModuleSource("anonParenCls", "export default (class {});");
            WScript.RegisterModuleSource("anonArrow", "export default () => {};");
            WScript.RegisterModuleSource("namedParenFn", "export default (function named() {});");
            testRunner.LoadModule(`
                import fn from "anonFn";
                import gen from "anonGen";
                import afn from "anonAsync";
                import pfn from "anonParenFn";
                import pgen from "anonParenGen";
                import pcls from "anonParenCls";
                import arr from "anonArrow";
                import nfn from "namedParenFn";
                assert.areEqual("default", fn.name);
                assert.areEqual("default", gen.name);
                assert.areEqual("default", afn.name);
                assert.areEqual("default", pfn.name);
                assert.areEqual("default", pgen.name);
                assert.areEqual("default", pcls.name);
                assert.areEqual("default", arr.name);
                assert.areEqual("named", nfn.name);
            `);

            function q() { return "xy"; }
            var o = {};
            o[q()] = class {};
            o[q] = class {};
            o[0] = class {};
            o["ab"] = class {};
            var o2 = {};
            o2[q] = class NamedCls {};
            assert.areEqual("", o.xy.name, "o[q()] = class {} has empty name");
            assert.areEqual("", o[q].name, "o[q] = class {} has empty name");
            assert.areEqual("", o[0].name, "o[0] = class {} has empty name");
            assert.areEqual("", o.ab.name, "o['ab'] = class {} has empty name");
            assert.areEqual("NamedCls", o2[q].name, "named class expression preserves its name");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
