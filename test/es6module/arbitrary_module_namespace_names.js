//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// ES2025 Arbitrary Module Namespace Identifier Names (StringLiteral ModuleExportName)

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function assertModuleSyntaxError(source, message) {
    let caught = false;
    try {
        testRunner.LoadModule(source, "samethread", true);
    } catch (e) {
        caught = true;
        assert.areEqual(e.constructor.toString(), SyntaxError.toString(), message);
    }
    assert.isTrue(caught, `Expected SyntaxError not thrown: ${message}`);
}

var tests = [
    {
        name: "Valid string-literal export/import names including Unicode, empty, spaces, surrogate pairs, and embedded NULs",
        body() {
            WScript.RegisterModuleSource("amnn_fixture.js", `
                function Mercury() { return "mercury"; }
                const emptyVal = "empty";
                const spaceVal = "with space";
                const moonVal = "moon";
                const nulB = "nul-b";
                const nulC = "nul-c";
                const plainA = "plain-a";
                export {
                    Mercury,
                    Mercury as "☿",
                    emptyVal as "",
                    spaceVal as "a b",
                    moonVal as "\\u{1F319}",
                    nulB as "a\\0b",
                    nulC as "a\\0c",
                    plainA as "a"
                };
                export default "default-val";
            `);

            WScript.RegisterModuleSource("amnn_reexport.js", `
                export { "☿", "", "a\\0b", "a\\0c", "a" } from "amnn_fixture.js";
                export { "☿" as "Ami", Mercury as "Sailor Mercury" } from "amnn_fixture.js";
                export { "\\uD83C\\uDF19" as moonFromPair } from "amnn_fixture.js";
                export * as "All" from "amnn_fixture.js";
                export * as "🌙" from "amnn_fixture.js";
            `);

            testRunner.LoadModule(`
                import {
                    "☿" as m,
                    "" as empty,
                    "a b" as space,
                    "🌙" as moon,
                    "a\\0b" as nb,
                    "a\\0c" as nc,
                    "a" as pa
                } from "amnn_fixture.js";
                import * as fx from "amnn_fixture.js";
                import * as re from "amnn_reexport.js";
                import { "Ami" as ami, "Sailor Mercury" as sm, moonFromPair, "All" as allNs, "🌙" as moonNs } from "amnn_reexport.js";

                assert.areEqual("mercury", m());
                assert.areEqual("empty", empty);
                assert.areEqual("with space", space);
                assert.areEqual("moon", moon);
                assert.areEqual("nul-b", nb);
                assert.areEqual("nul-c", nc);
                assert.areEqual("plain-a", pa);

                assert.areEqual(fx.Mercury, fx["☿"]);
                assert.areEqual("empty", fx[""]);
                assert.areEqual("with space", fx["a b"]);
                assert.areEqual("moon", fx["\\uD83C\\uDF19"]);
                assert.areEqual("nul-b", fx["a\\0b"]);
                assert.areEqual("nul-c", fx["a\\0c"]);
                assert.areEqual("plain-a", fx["a"]);
                assert.areEqual("default-val", fx.default);

                assert.areEqual("mercury", ami());
                assert.areEqual("mercury", sm());
                assert.areEqual("moon", moonFromPair);
                assert.areEqual(fx["☿"], re["☿"]);
                assert.areEqual("empty", re[""]);
                assert.areEqual("nul-b", re["a\\0b"]);
                assert.areEqual("nul-c", re["a\\0c"]);
                assert.areEqual("plain-a", re["a"]);
                assert.areEqual(fx, allNs);
                assert.areEqual(fx, moonNs);
                assert.areEqual("mercury", re.All["☿"]());
                assert.areEqual("default-val", re.All.default);
            `);
        }
    },
    {
        name: "Local variable with same name as re-exported specifier is not treated as local module export",
        body() {
            WScript.RegisterModuleSource("amnn_local_collide_dep.js", `
                export const x = "from-dep";
                export const z = "z-from-dep";
                export { z as "localOnly" };
            `);

            WScript.RegisterModuleSource("amnn_local_collide_mid.js", `
                const x = "local-x";
                const localOnly = "local-only";
                export { x as "reexpX" } from "amnn_local_collide_dep.js";
                export { "localOnly" as "reexpLocalOnly" } from "amnn_local_collide_dep.js";
                export function readLocals() { return x + ":" + localOnly; }
            `);

            testRunner.LoadModule(`
                import { reexpX, reexpLocalOnly, readLocals } from "amnn_local_collide_mid.js";
                assert.areEqual("from-dep", reexpX);
                assert.areEqual("z-from-dep", reexpLocalOnly);
                assert.areEqual("local-x:local-only", readLocals());
            `);
        }
    },
    {
        name: "Literal '*' export/import does not collide with namespace-star",
        body() {
            WScript.RegisterModuleSource("amnn_star_dep.js", `
                const starVal = "literal-star";
                export { starVal as "*" };
                export const extra = "extra-val";
                export default "dep-default";
            `);

            WScript.RegisterModuleSource("amnn_star_empty.js", `
                export const onlyOne = 1;
            `);

            WScript.RegisterModuleSource("amnn_star_reexp_all.js", `
                export * from "amnn_star_dep.js";
            `);

            WScript.RegisterModuleSource("amnn_star_reexp_named.js", `
                export { "*" } from "amnn_star_dep.js";
                export { "*" as starAlias } from "amnn_star_dep.js";
            `);

            WScript.RegisterModuleSource("amnn_star_reexp_extra_as_star.js", `
                export { extra as "*" } from "amnn_star_dep.js";
            `);

            WScript.RegisterModuleSource("amnn_star_ns_as_star.js", `
                export * as "*" from "amnn_star_dep.js";
            `);

            WScript.RegisterModuleSource("amnn_star_local_ns.js", `
                import * as depNs from "amnn_star_dep.js";
                export { depNs as "*" };
            `);

            testRunner.LoadModule(`
                import { "*" as litStar } from "amnn_star_dep.js";
                import * as depNs from "amnn_star_dep.js";
                import { "*" as reexpAllStar, extra } from "amnn_star_reexp_all.js";
                import * as reexpAllNs from "amnn_star_reexp_all.js";
                import { "*" as reexpNamedStar, starAlias } from "amnn_star_reexp_named.js";
                import { "*" as reexpExtraStar } from "amnn_star_reexp_extra_as_star.js";
                import { "*" as nsAsStar } from "amnn_star_ns_as_star.js";
                import { "*" as localNsStar } from "amnn_star_local_ns.js";

                assert.areEqual("literal-star", litStar);
                assert.areEqual("literal-star", depNs["*"]);
                assert.areEqual("literal-star", reexpAllStar);
                assert.areEqual("extra-val", extra);
                assert.areEqual("literal-star", reexpAllNs["*"]);
                assert.isFalse("default" in reexpAllNs, "export * skips default");
                assert.areEqual("literal-star", reexpNamedStar);
                assert.areEqual("literal-star", starAlias);
                assert.areEqual("extra-val", reexpExtraStar);
                assert.areEqual(depNs, nsAsStar);
                assert.areEqual("literal-star", nsAsStar["*"]);
                assert.areEqual(depNs, localNsStar);
            `);

            // Missing literal "*" must fail resolution, not silently resolve to namespace.
            WScript.RegisterModuleSource("amnn_star_missing_import.js", 'import { "*" as x } from "amnn_star_empty.js";');
            WScript.RegisterModuleSource("amnn_star_missing_export.js", 'export { "*" as x } from "amnn_star_empty.js";');

            testRunner.LoadModule(`
                Promise.all([
                    import("amnn_star_missing_import.js").then(
                        () => { assert.fail("import missing '*' should fail"); },
                        (e) => { assert.areEqual(e.constructor.toString(), SyntaxError.toString()); }
                    ),
                    import("amnn_star_missing_export.js").then(
                        () => { assert.fail("export missing '*' should fail"); },
                        (e) => { assert.areEqual(e.constructor.toString(), SyntaxError.toString()); }
                    )
                ]).then(
                    () => { _asyncExit(); },
                    (e) => { assert.fail("Unexpected error: " + e); _asyncExit(); }
                );
            `, "samethread", false, true);
        }
    },
    {
        name: "Integer-index export names on ModuleNamespace",
        body() {
            WScript.RegisterModuleSource("amnn_index_fixture.js", `
                var a = 10;
                var b = 20;
                var c = 30;
                var d = 40;
                var maxIdx = 50;
                var nonIdx = 60;
                export {
                    a as "0",
                    b as "1",
                    c as "10",
                    d as "2",
                    maxIdx as "4294967294",
                    nonIdx as "4294967295"
                };
            `);

            WScript.RegisterModuleSource("amnn_index_reexport.js", `
                export { "0", "1" as "3" } from "amnn_index_fixture.js";
            `);

            testRunner.LoadModule(`
                import * as ns from "amnn_index_fixture.js";
                import * as reNs from "amnn_index_reexport.js";

                assert.areEqual(10, ns[0]);
                assert.areEqual(10, ns["0"]);
                assert.areEqual(20, Reflect.get(ns, 1));
                assert.areEqual(30, ns[10]);
                assert.areEqual(40, ns[2]);
                assert.areEqual(50, ns[4294967294]);
                assert.areEqual(60, ns[4294967295]);
                assert.isUndefined(ns[99]);

                assert.areEqual(10, reNs[0]);
                assert.areEqual(20, reNs[3]);
                assert.isUndefined(reNs[1]);

                assert.throws(() => { ns[0] = 1; }, TypeError);
                assert.isFalse(Reflect.set(ns, 1, 1));
                assert.throws(() => { ns[99] = 2; }, TypeError);
                assert.isFalse(Reflect.set(ns, 99, 2));
                assert.areEqual(10, ns[0]);
                assert.areEqual(20, ns[1]);

                assert.isTrue(0 in ns);
                assert.isTrue(Reflect.has(ns, 1));
                assert.isTrue(4294967294 in ns);
                assert.isFalse(99 in ns);
                assert.isTrue(Object.prototype.hasOwnProperty.call(ns, 0));
                assert.isTrue(Object.hasOwn(ns, 1));
                assert.isFalse(Object.hasOwn(ns, 99));

                assert.throws(() => { delete ns[0]; }, TypeError);
                assert.isFalse(Reflect.deleteProperty(ns, 1));
                assert.isTrue(delete ns[99]);
                assert.isTrue(Reflect.deleteProperty(ns, 99));

                assert.throws(() => { delete reNs[0]; }, TypeError);
                assert.isFalse(Reflect.deleteProperty(reNs, 3));
                assert.isTrue(delete reNs[1]);

                const d0 = Object.getOwnPropertyDescriptor(ns, "0");
                assert.areEqual(10, d0.value);
                assert.isTrue(d0.writable);
                assert.isTrue(d0.enumerable);
                assert.isFalse(d0.configurable);

                const d1 = Object.getOwnPropertyDescriptor(ns, 1);
                assert.areEqual(20, d1.value);
                assert.isUndefined(Object.getOwnPropertyDescriptor(ns, 99));

                // ES2025 ModuleNamespace [[OwnPropertyKeys]]: lexicographic UTF-16 order, then symbols.
                const expectedNames = ["0", "1", "10", "2", "4294967294", "4294967295"];
                assert.areEqual(expectedNames, Object.getOwnPropertyNames(ns));
                assert.areEqual(expectedNames, Object.keys(ns));
                assert.areEqual(expectedNames.concat([Symbol.toStringTag]), Reflect.ownKeys(ns));
            `);
        }
    },
    {
        name: "Integer-index export TDZ semantics on ModuleNamespace",
        body() {
            let tdzObserved = false;
            WScript.RegisterModuleSource("amnn_tdz_entry.js", `
                import "amnn_tdz_reader.js";
                export let late = 777;
                export { late as "0" };
            `);
            WScript.RegisterModuleSource("amnn_tdz_reader.js", `
                import * as ns from "amnn_tdz_entry.js";
                assert.isTrue(0 in ns, "0 in ns is true even in TDZ");
                assert.throws(() => ns[0], ReferenceError, "reading TDZ index throws ReferenceError");
                assert.throws(() => Object.getOwnPropertyDescriptor(ns, 0), ReferenceError, "descriptor read in TDZ throws ReferenceError");
                assert.throws(() => { ns[0] = 1; }, TypeError, "setting TDZ index throws TypeError, not ReferenceError");
                assert.throws(() => { delete ns[0]; }, TypeError, "deleting TDZ index throws TypeError, not ReferenceError");
            `);
            try {
                testRunner.LoadModule('import "amnn_tdz_entry.js";');
                tdzObserved = true;
            } catch (e) {
                assert.fail("Unexpected error during TDZ checks: " + e);
            }
            assert.isTrue(tdzObserved);
        }
    },
    {
        name: "Early SyntaxErrors for ill-formed Unicode and invalid ModuleExportName grammar",
        body() {
            // Unpaired high / low surrogates
            assertModuleSyntaxError('let Foo = 1; export { Foo as "\\uD83D" };', "unpaired high surrogate in export");
            assertModuleSyntaxError('let Foo = 1; export { Foo as "\\uDC00" };', "unpaired low surrogate in export");
            assertModuleSyntaxError('let Foo = 1; export { Foo as "\\uDC00\\uD800" };', "reversed surrogate pair in export");
            assertModuleSyntaxError('let Foo = 1; export { Foo as "\\uD800\\uD800\\uDC00" };', "unpaired high before valid pair");
            assertModuleSyntaxError('let Foo = 1; export { Foo as "\\uD83D\\uDE00\\uD83D" };', "valid pair then unpaired high");
            assertModuleSyntaxError('export { "\\uD83D" } from "amnn_fixture.js";', "unpaired high in re-export");
            assertModuleSyntaxError('export { "☿" as "\\uD83D" } from "amnn_fixture.js";', "unpaired high in re-export as");
            assertModuleSyntaxError('import { "\\uD83D" as x } from "amnn_fixture.js";', "unpaired high in import");
            assertModuleSyntaxError('export * as "\\uD83D" from "amnn_fixture.js";', "unpaired high in export * as");
            assertModuleSyntaxError('export "*" as "\\uD83D" from "amnn_fixture.js";', "quoted star before as");

            // Strict-mode octal escape in ModuleExportName
            assertModuleSyntaxError('let a = 1; export { a as "\\01" };', "octal escape in export string");
            assertModuleSyntaxError('import { "\\1" as a } from "amnn_fixture.js";', "octal escape in import string");

            // ImportSpecifier requires `as ImportedBinding` when first token is StringLiteral or reserved word
            assertModuleSyntaxError('import { "a" } from "amnn_fixture.js";', "import string without as");
            assertModuleSyntaxError('import { a as "b" } from "amnn_fixture.js";', "import as string");
            assertModuleSyntaxError('import { "a" as "b" } from "amnn_fixture.js";', "import string as string");
            assertModuleSyntaxError('import { default } from "amnn_fixture.js";', "import reserved word without as");

            // ExportDeclaration without FromClause cannot reference a StringLiteral binding
            assertModuleSyntaxError('export { "a" };', "local export standalone string");
            assertModuleSyntaxError('export { "a" as b };', "local export string as ident");
            assertModuleSyntaxError('function foo() {} export { "foo" as "bar" };', "local export string as string even when binding exists");
            assertModuleSyntaxError('let a = 1; export { "a" as b };', "local export string as ident even when binding exists");

            // Duplicate exports & syntax
            assertModuleSyntaxError('let a = 1, b = 2; export { a as "dup", b as "dup" };', "duplicate string export");
            assertModuleSyntaxError('let a = 1; export { a as "default" }; export default 2;', "duplicate default export");
            assertModuleSyntaxError('let a = 1, b = 2; export { a as "x\\0y", b as "x\\0y" };', "duplicate embedded-NUL export");
            assertModuleSyntaxError('let a = 1, b = 2; export { a as "x" b };', "missing comma after string export");
            assertModuleSyntaxError('import { "a" \\u0061s x } from "amnn_fixture.js";', "escaped as in import");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
