//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Regression test for unnamed module / eval uncoercible exception formatting
// in WScriptJsrt::PrintException, AutoString::Initialize, and LoadScriptHelper cleanup.

// 1. Unnamed WScript.LoadScript(..., "module") throwing an uncoercible object.
WScript.LoadScript("throw { toString() { throw 1; }, message: 'boom-loadscript-module' };", "module");

// 2. Unnamed WScript.LoadModule(...) throwing an uncoercible object.
WScript.LoadModule("throw { toString() { throw 1; }, message: 'boom-loadmodule' };");

// 3. Unnamed module executing eval(...) throwing an uncoercible object.
WScript.LoadModule("eval(\"throw { toString() { throw 1; }, message: 'boom-eval-module' };\");");

// 4. Unnamed module compile error and LoadScriptHelper argument coercion error cleanup paths.
try {
    WScript.LoadScript("const = ;", "module");
} catch (e) {
}

try {
    WScript.LoadScript("1;", { toString() { throw new Error("bad inject type"); } });
} catch (e) {
}

try {
    WScript.LoadScript("1;", "module", { toString() { throw new Error("bad file name"); } });
} catch (e) {
}

print("pass");
