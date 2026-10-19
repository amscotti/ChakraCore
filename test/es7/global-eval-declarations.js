//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// GlobalDeclarationInstantiation and EvalDeclarationInstantiation must reject
// var/function collisions with global lexical declarations, restricted
// (non-configurable) globals, and lower lexical environments before executing
// any body statement. Global function/var bindings also have to pass
// CanDeclareGlobalFunction / CanDeclareGlobalVar; global lexical bindings are
// independent of global-object extensibility.

var global = this;

function loadScript(source) {
    WScript.LoadScript(source);
}

function assertScriptSyntaxError(source, message) {
    assert.throws(function () { loadScript(source); }, SyntaxError, message);
}

function assertScriptTypeError(source, message) {
    assert.throws(function () { loadScript(source); }, TypeError, message);
}

var tests = [
    {
        name: "global lexical declarations reject restricted and declared globals",
        body() {
            Object.defineProperty(global, "geConfigurable", {
                value: 0,
                writable: false,
                enumerable: false,
                configurable: true
            });
            loadScript("let geConfigurable = 1;");
            assert.strictEqual(geConfigurable, 1, "configurable property is shadowed by a lexical declaration");

            Object.defineProperty(global, "geRestricted", {
                value: 0,
                writable: false,
                enumerable: false,
                configurable: false
            });
            assertScriptSyntaxError(
                "var geRestrictedSideEffect; let geRestricted;",
                "non-configurable global property is restricted");
            assert.strictEqual(typeof geRestrictedSideEffect, "undefined",
                "failed global instantiation creates no var binding");

            loadScript("var geDeclaredVar;");
            assertScriptSyntaxError(
                "var geDeclaredVarSideEffect; let geDeclaredVar;",
                "non-configurable var declaration is restricted");
            assert.strictEqual(typeof geDeclaredVarSideEffect, "undefined",
                "failed global instantiation creates no sibling var");

            loadScript("function geDeclaredFn() {}");
            assertScriptSyntaxError(
                "var geDeclaredFnSideEffect; let geDeclaredFn;",
                "non-configurable function declaration is restricted");
            assert.strictEqual(typeof geDeclaredFnSideEffect, "undefined",
                "failed global instantiation creates no sibling var");
        }
    },
    {
        name: "global var and function declarations reject existing lexical bindings",
        body() {
            loadScript("var geVarOk; let geLetBinding = 1;");
            assertScriptSyntaxError(
                "var geVarSideEffect; var geLetBinding;",
                "var declaration collides with a lexical binding");
            assert.strictEqual(typeof geVarSideEffect, "undefined",
                "failed global instantiation creates no var binding");

            assertScriptSyntaxError(
                "var geFnSideEffect; function geLetBinding() {}",
                "function declaration collides with a lexical binding");
            assert.strictEqual(typeof geFnSideEffect, "undefined",
                "failed global instantiation creates no sibling var");

            loadScript("function geFnOk() {}");
            assertScriptSyntaxError(
                "var geFnOkSideEffect; let geFnOk;",
                "lexical declaration collides with a function declaration");
            assert.strictEqual(typeof geFnOkSideEffect, "undefined",
                "failed global instantiation creates no sibling var");
        }
    },
    {
        name: "eval-introduced global vars and functions remain shadowable",
        body() {
            loadScript('eval("var geEvalVar;");');
            loadScript("let geEvalVar = 1;");
            assert.strictEqual(geEvalVar, 1, "eval var is configurable and can be shadowed");

            loadScript('eval("function geEvalFn() {}");');
            loadScript("const geEvalFn = 2;");
            assert.strictEqual(geEvalFn, 2, "eval function is configurable and can be shadowed");
        }
    },
    {
        name: "global function declarations preserve non-configurable descriptors",
        body() {
            Object.defineProperty(global, "geExistingFn", {
                value: 1,
                writable: true,
                enumerable: true,
                configurable: false
            });
            loadScript("function geExistingFn() { return 3; }");
            assert.strictEqual(geExistingFn(), 3, "non-configurable writable/enumerable function is updated");
            var descriptor = Object.getOwnPropertyDescriptor(global, "geExistingFn");
            assert.strictEqual(descriptor.configurable, false, "configurable is preserved");
            assert.strictEqual(descriptor.writable, true, "writable is preserved");
            assert.strictEqual(descriptor.enumerable, true, "enumerable is preserved");

            Object.defineProperty(global, "geBadFn", {
                value: 1,
                writable: true,
                enumerable: false,
                configurable: false
            });
            assertScriptTypeError(
                "function geBadFn() {}",
                "non-configurable non-enumerable property rejects a function declaration");
            assert.strictEqual(geBadFn, 1, "rejected function declaration leaves the property untouched");
        }
    },
    {
        name: "direct and indirect eval reject lower and global lexical collisions",
        body() {
            assert.throws(function () {
                (function () {
                    let geLowerLex;
                    eval("var geLowerLex;");
                }());
            }, SyntaxError, "direct eval var collides with a block lexical binding");

            assert.throws(function () {
                (function () {
                    let geBodyLex = 1;
                    eval("var geBodyLex;");
                }());
            }, SyntaxError, "direct eval var collides with a function-body lexical binding");

            loadScript("let geGlobalLex = 1;");
            assert.throws(function () {
                (0, eval)("var geGlobalLex;");
            }, SyntaxError, "indirect eval var collides with a global lexical binding");
            assert.strictEqual(geGlobalLex, 1, "failed indirect eval leaves the lexical value intact");

            assert.throws(function () {
                loadScript('let geDirectGlobalLex = 1; eval("var geDirectGlobalLex;");');
            }, SyntaxError, "direct eval var collides with a global lexical binding");
        }
    },
    {
        name: "eval function declarations preserve non-configurable global descriptors",
        body() {
            Object.defineProperty(global, "geEvalExistingFn", {
                value: "initial",
                writable: true,
                enumerable: true,
                configurable: false
            });
            eval('var geInitial = geEvalExistingFn; function geEvalExistingFn() { return 2222; }');
            assert.strictEqual(typeof geInitial, "function", "direct eval sees the hoisted function");
            assert.strictEqual(geInitial(), 2222, "direct eval function value is visible");
            assert.strictEqual(geEvalExistingFn(), 2222, "direct eval updates the existing property");
            var descriptor = Object.getOwnPropertyDescriptor(global, "geEvalExistingFn");
            assert.strictEqual(descriptor.configurable, false, "direct eval preserves configurable");
            assert.strictEqual(descriptor.writable, true, "direct eval preserves writable");
            assert.strictEqual(descriptor.enumerable, true, "direct eval preserves enumerable");

            Object.defineProperty(global, "geIndirectEvalFn", {
                value: "initial",
                writable: true,
                enumerable: true,
                configurable: false
            });
            (0, eval)('var globalThisProbe = this; var geIndirectInitial = geIndirectEvalFn; function geIndirectEvalFn() { return 3333; }');
            assert.strictEqual(typeof geIndirectInitial, "function", "indirect eval sees the hoisted function");
            assert.strictEqual(geIndirectInitial(), 3333, "indirect eval function value is visible");
            assert.strictEqual(geIndirectEvalFn(), 3333, "indirect eval updates the existing property");
            descriptor = Object.getOwnPropertyDescriptor(global, "geIndirectEvalFn");
            assert.strictEqual(descriptor.configurable, false, "indirect eval preserves configurable");
            assert.strictEqual(descriptor.writable, true, "indirect eval preserves writable");
            assert.strictEqual(descriptor.enumerable, true, "indirect eval preserves enumerable");
        }
    },
    {
        name: "eval Annex B block functions do not reinitialize existing global bindings",
        body() {
            Object.defineProperty(global, "geAnnexBEvalFn", {
                value: "original",
                writable: true,
                enumerable: false,
                configurable: true
            });
            (0, eval)(
                "var geAnnexBSeen = geAnnexBEvalFn;" +
                "{ function geAnnexBEvalFn() {} }");
            assert.strictEqual(geAnnexBSeen, "original", "synthetic var does not reinitialize the property");
            assert.strictEqual(typeof geAnnexBEvalFn, "function", "block function assignment updates the value");
            var descriptor = Object.getOwnPropertyDescriptor(global, "geAnnexBEvalFn");
            assert.strictEqual(descriptor.enumerable, false, "block function assignment preserves enumerable");
            assert.strictEqual(descriptor.writable, true, "block function assignment preserves writable");
            assert.strictEqual(descriptor.configurable, true, "block function assignment preserves configurable");
        }
    },
    {
        name: "eval Annex B block functions reject global lexical and skip local lexical bindings",
        body() {
            loadScript("let geAnnexBGlobalLet = 1; let geAnnexBGlobalSideEffect = false;");
            assert.throws(function () {
                loadScript(
                    "eval('geAnnexBGlobalSideEffect = true; if (true) { function geAnnexBGlobalLet() {} }');");
            }, SyntaxError, "global eval block function collides with a global lexical binding");
            assert.strictEqual(geAnnexBGlobalLet, 1, "global lexical value is not overwritten");
            assert.strictEqual(geAnnexBGlobalSideEffect, false, "eval body did not run");
            assert.strictEqual(Object.prototype.hasOwnProperty.call(global, "geAnnexBGlobalLet"), false,
                "global lexical binding is not a global-object property");

            (function () {
                let geAnnexBLocalLet = 1;
                let geAnnexBLocalSideEffect = false;
                eval("geAnnexBLocalSideEffect = true; if (true) { function geAnnexBLocalLet() {} }");
                assert.strictEqual(geAnnexBLocalLet, 1, "local let binding is unchanged");
                assert.strictEqual(geAnnexBLocalSideEffect, true, "local eval body still runs");
            }());

            (function () {
                const geAnnexBLocalConst = 1;
                let geAnnexBLocalConstSideEffect = false;
                eval("geAnnexBLocalConstSideEffect = true; if (true) { function geAnnexBLocalConst() {} }");
                assert.strictEqual(geAnnexBLocalConst, 1, "local const binding is unchanged");
                assert.strictEqual(geAnnexBLocalConstSideEffect, true, "local const eval body still runs");
            }());
        }
    },
    {
        name: "indirect eval Annex B global lexical and ordinary function descriptors",
        body() {
            loadScript("let geIndirectLet = 1; const geIndirectConst = 2; class geIndirectClass {}");
            assert.throws(function () {
                loadScript('(0, eval)("geIndirectSide = true; if (true) { function geIndirectLet() {} }");');
            }, SyntaxError, "indirect eval block function collides with a global let");
            assert.strictEqual(geIndirectLet, 1, "indirect eval did not overwrite the global let");
            assert.strictEqual(Object.prototype.hasOwnProperty.call(global, "geIndirectLet"), false,
                "global let is not a property");
            assert.strictEqual(typeof geIndirectSide, "undefined", "indirect eval body did not run");
            assert.throws(function () {
                loadScript('(0, eval)("if (true) { function geIndirectConst() {} }");');
            }, SyntaxError, "indirect eval block function collides with a global const");
            assert.strictEqual(geIndirectConst, 2, "global const is unchanged");
            assert.throws(function () {
                loadScript('(0, eval)("if (true) { function geIndirectClass() {} }");');
            }, SyntaxError, "indirect eval block function collides with a global class");
            assert.strictEqual(typeof geIndirectClass, "function", "global class is unchanged");

        }
    },
    {
        name: "Annex B eval block-function stores use SetMutableBinding semantics",
        body() {
            loadScript('Object.defineProperty(this, "geAnnexBNonWritable", { value: "original", writable: false, enumerable: false, configurable: false });');
            loadScript('eval("if (true) { function geAnnexBNonWritable() {} }");');
            assert.strictEqual(geAnnexBNonWritable, "original", "non-writable data property is unchanged");
            var descriptor = Object.getOwnPropertyDescriptor(global, "geAnnexBNonWritable");
            assert.strictEqual(descriptor.writable, false, "non-writable data property remains non-writable");
            assert.strictEqual(descriptor.configurable, false, "non-writable data property remains non-configurable");

            loadScript('Object.defineProperty(this, "geAnnexBAccessorDirect", { get: function () { return "get"; }, set: function (v) { this.geAnnexBAccessorDirectSet = v; }, enumerable: false, configurable: true }); eval("if (true) { function geAnnexBAccessorDirect() {} }");');
            assert.strictEqual(geAnnexBAccessorDirect, "get", "direct eval leaves the getter unchanged");
            assert.strictEqual(typeof geAnnexBAccessorDirectSet, "function", "direct eval invokes the setter");

            loadScript('Object.defineProperty(this, "geAnnexBAccessorIndirect", { get: function () { return "get"; }, set: function (v) { this.geAnnexBAccessorIndirectSet = v; }, enumerable: false, configurable: true }); (0, eval)("if (true) { function geAnnexBAccessorIndirect() {} }");');
            assert.strictEqual(geAnnexBAccessorIndirect, "get", "indirect eval leaves the getter unchanged");
            assert.strictEqual(typeof geAnnexBAccessorIndirectSet, "function", "indirect eval invokes the setter");

            loadScript('Object.defineProperty(this, "geAnnexBWritable", { value: "original", writable: true, enumerable: false, configurable: true }); eval("if (true) { function geAnnexBWritable() {} }");');
            assert.strictEqual(typeof geAnnexBWritable, "function", "writable data property is updated");
            descriptor = Object.getOwnPropertyDescriptor(global, "geAnnexBWritable");
            assert.strictEqual(descriptor.enumerable, false, "writable data property keeps enumerable");
            assert.strictEqual(descriptor.writable, true, "writable data property keeps writable");
            assert.strictEqual(descriptor.configurable, true, "writable data property keeps configurable");
        }
    },
    {
        name: "CanDeclareGlobalFunction precedes the Annex B synthetic preflight",
        body() {
            Object.defineProperty(global, "geOrderNonDefinable", {
                value: 1, writable: true, enumerable: false, configurable: false
            });
            loadScript("let geOrderSyntheticLexical = 1;");
            loadScript(
                "var geOrderCaught = 'none'; var geOrderSide = false;" +
                "try { eval('function geOrderNonDefinable() {} geOrderSide = true; if (true) { function geOrderSyntheticLexical() {} }'); }" +
                "catch (e) { geOrderCaught = e.name; }");
            assert.strictEqual(geOrderCaught, "TypeError",
                "non-definable function TypeError precedes the synthetic lexical check");
            assert.strictEqual(geOrderSide, false, "eval body did not run");
            assert.strictEqual(geOrderNonDefinable, 1, "non-definable binding is unchanged");
            assert.strictEqual(geOrderSyntheticLexical, 1, "synthetic collision binding is unchanged");
        }
    },
    {
        name: "direct eval runs CanDeclareGlobalFunction before creating any function",
        body() {
            Object.defineProperty(global, "geNonDefinableFn", {
                value: 1,
                writable: true,
                enumerable: false,
                configurable: false
            });
            loadScript(
                "var geNonDefinableCaught = 'none';" +
                "try { eval('function geEarlierFn() {} function geNonDefinableFn() {}'); }" +
                "catch (e) { geNonDefinableCaught = e.name; }");
            assert.strictEqual(geNonDefinableCaught, "TypeError",
                "non-configurable non-enumerable function binding is rejected");
            assert.strictEqual(Object.prototype.hasOwnProperty.call(global, "geEarlierFn"), false,
                "earlier function was not created before the failed preflight");
            assert.strictEqual(geNonDefinableFn, 1, "existing non-definable binding is unchanged");

            loadScript("let geMaskedLexical = 1;");
            Object.defineProperty(global, "geMaskedBadFn", {
                value: 2,
                writable: true,
                enumerable: false,
                configurable: false
            });
            loadScript(
                "var geMaskedCaught = 'none'; var geMaskedSideEffect = false;" +
                "try { eval('function geMaskedBadFn() {} geMaskedSideEffect = true; var geMaskedLexical;'); }" +
                "catch (e) { geMaskedCaught = e.name; }");
            assert.strictEqual(geMaskedCaught, "SyntaxError",
                "later lexical collision is checked before CanDeclareGlobalFunction");
            assert.strictEqual(geMaskedSideEffect, false, "eval body did not run after the lexical collision");
            assert.strictEqual(geMaskedBadFn, 2, "non-definable function binding was not touched");
            assert.strictEqual(geMaskedLexical, 1, "existing global lexical binding is unchanged");

            (function () {
                eval("function undefined() {}");
                assert.strictEqual(typeof undefined, "function",
                    "local eval does not run the global CanDeclareGlobalFunction preflight");
            }());
        }
    },
    {
        name: "parameter-default direct eval rejects var collisions before the body",
        body() {
            var callCount = 0;
            function geParamFn(a = eval("var a = 42")) {
                callCount++;
            }
            assert.throws(function () { geParamFn(); }, SyntaxError,
                "function default parameter eval var collides with the parameter");
            assert.strictEqual(callCount, 0, "function body did not run");

            var arrowCallCount = 0;
            var geParamArrow = (a = eval("var a = 42")) => { arrowCallCount++; };
            assert.throws(function () { geParamArrow(); }, SyntaxError,
                "arrow default parameter eval var collides with the parameter");
            assert.strictEqual(arrowCallCount, 0, "arrow body did not run");

            var methodCallCount = 0;
            var geParamObject = {
                method(a = eval("var a = 42")) {
                    methodCallCount++;
                }
            };
            assert.throws(function () { geParamObject.method(); }, SyntaxError,
                "method default parameter eval var collides with the parameter");
            assert.strictEqual(methodCallCount, 0, "method body did not run");

            var noCollisionCount = 0;
            function geNoCollision(a = eval("var geSeparateEvalVar = 42; geSeparateEvalVar")) {
                noCollisionCount++;
            }
            assert.strictEqual(geNoCollision(), undefined, "non-colliding eval var in a default parameter is allowed");
            assert.strictEqual(noCollisionCount, 1, "non-colliding function body ran");
        }
    },
    {
        name: "global lexical bindings work when the global object is non-extensible",
        body() {
            Object.preventExtensions(global);

            assertScriptTypeError(
                "geNonExtensibleFnExecuted = true; function geNonExtensibleFn() {}",
                "function declaration is rejected on a non-extensible global");
            assert.strictEqual(typeof geNonExtensibleFnExecuted, "undefined",
                "function body did not run after the rejected declaration");

            assertScriptTypeError(
                "geNonExtensibleVarExecuted = true; var geNonExtensibleVar;",
                "var declaration is rejected on a non-extensible global");
            assert.strictEqual(typeof geNonExtensibleVarExecuted, "undefined",
                "var body did not run after the rejected declaration");

            loadScript("let geNonExtensibleLet = 1; const geNonExtensibleConst = 2; class geNonExtensibleClass {}");
            assert.strictEqual(geNonExtensibleLet, 1, "global let binding is created");
            geNonExtensibleLet = 3;
            assert.strictEqual(geNonExtensibleLet, 3, "global let binding is mutable");
            assert.throws(function () { geNonExtensibleConst = 3; }, TypeError,
                "global const binding is immutable");
            assert.strictEqual(geNonExtensibleConst, 2, "global const value is unchanged");
            assert.strictEqual(typeof geNonExtensibleClass, "function", "global class binding is created");
            assert.strictEqual(Object.prototype.hasOwnProperty.call(global, "geNonExtensibleLet"), false,
                "global let binding is not a global-object property");
            assert.strictEqual(Object.prototype.hasOwnProperty.call(global, "geNonExtensibleConst"), false,
                "global const binding is not a global-object property");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
