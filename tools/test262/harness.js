//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
'use strict';

// Keep the upstream harness/agent implementation, adding the native host hooks
// present in this fork. Versions and transitive dependencies are lockfile-pinned.
const ChakraAgent = require('eshost/lib/agents/chakra');
ChakraAgent.runtime = ChakraAgent.runtime.replace(/Date\.now\(\)/g, 'WScript.monotonicNow()') + `
delete $262.IsHTMLDDA;
$262.detachArrayBuffer = function(buffer) {
    // SerializeObject performs JsDetachArrayBuffer; Deserialize claims/releases
    // the transferred storage (bin/ch/WScriptJsrt.cpp:308-405,443-485).
    WScript.Deserialize(WScript.SerializeObject(buffer, [buffer]));
};
`;

const spawn = ChakraAgent.prototype.createChildProcess;
ChakraAgent.prototype.createChildProcess = async function(...args) {
    const child = await spawn.apply(this, args);
    this.exitStatus = {};
    if (child) {
        child.on('close', (code, signal) => { this.exitStatus = { code, signal }; });
    }
    return child;
};
const evaluate = ChakraAgent.prototype.evalScript;
ChakraAgent.prototype.evalScript = async function(...args) {
    const result = await evaluate.apply(this, args);
    result.exitStatus = this.exitStatus;
    return result;
};

const validatorPath = require.resolve('test262-harness/lib/validator');
const validate = require(validatorPath);
require.cache[validatorPath].exports = function(test) {
    const status = test.rawResult.exitStatus || {};
    if (status.signal || (status.code != null && status.code !== 0)) {
        return { pass: false, message: `Host exited abnormally: ${JSON.stringify(status)}` };
    }
    return validate(test);
};

require('test262-harness/bin/run');
