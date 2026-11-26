//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Focused regression test for SourceTextModuleRecord::ParseSource error-path cleanup
// when PostParseProcess encounters an already-recorded child/export error after notifying host.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

WScript.RegisterModuleSource("mod_missing_dep_parent.js", `
    import { x } from "mod_does_not_exist_12345.js";
    export const y = x;
`);

WScript.RegisterModuleSource("mod_bad_export_child.js", `
    export const a = 1;
`);
WScript.RegisterModuleSource("mod_bad_export_parent.js", `
    import { missingSymbol } from "mod_bad_export_child.js";
    export const z = missingSymbol;
`);

WScript.RegisterModuleSource("mod_bad_export_child2.js", `
    export const b = 2;
`);
WScript.RegisterModuleSource("mod_bad_export_parent2.js", `
    import { missingSymbol2 } from "mod_bad_export_child2.js";
    export const w = missingSymbol2;
`);
WScript.RegisterModuleSource("mod_multi_parent2.js", `
    import { w } from "mod_bad_export_parent2.js";
    export default w;
`);

let asyncEntered = 0;
let asyncExited = 0;
let passed = 0;

function checkDynamicFail(specifier, expectedErrName) {
    asyncEntered++;
    import(specifier).then(
        () => {
            throw new Error("Expected dynamic import to reject for " + specifier);
        },
        (err) => {
            assert.isTrue(err instanceof Error || (err && typeof err.name === "string"), "rejection is an error for " + specifier);
            if (expectedErrName) {
                assert.areEqual(expectedErrName, err.name, "error name for " + specifier);
            }
            passed++;
            asyncExited++;
        }
    );
}

// Nested child resolution/export link errors via dynamic import (triggers ParseSource + PostParseProcess
// cleanup + ResolveOrRejectDynamicImportPromise + NotifyParentsAsNeeded).
checkDynamicFail("mod_missing_dep_parent.js", "URIError");
checkDynamicFail("mod_bad_export_parent.js", "SyntaxError");
checkDynamicFail("mod_multi_parent2.js", "SyntaxError");

// Also verify nested samethread LoadModule when child has link error does not corrupt caller realm.
WScript.RegisterModuleSource("mod_bad_export_parent_st.js", `
    import { missingSymbol3 } from "mod_bad_export_child.js";
    export const s = missingSymbol3;
`);
WScript.LoadModuleFile("mod_bad_export_parent_st.js", "samethread");

WScript.SetTimeout(() => {
    assert.areEqual(asyncEntered, asyncExited, "all dynamic import failure promises settled");
    assert.areEqual(3, passed, "all 3 failing dynamic imports rejected cleanly");
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
    print("PASS");
}, 50);
