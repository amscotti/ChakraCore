//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
  {
    // E7 early-error lane: an ImportCall is an ~invalid~ assignment target
    // in every mode (no Annex B.3.9 web-compat production covers it), so
    // `import(...) *= 2` is a parse-time SyntaxError (Node-verified), not a
    // runtime error. The offending source must come through eval so the
    // parse error is observable without failing the whole test file.
    name: "Dynamic import can be used as part of an op-equiv expression",
    body: function () {
        var failed = false;
        try {
          function foo() { };
          foo(eval("import('ModuleSimpleExport.js')*=2"));
        } catch(e) { 
          failed = true;
        }
        assert.isTrue(failed, "Failed as expected.")
    }
  },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
