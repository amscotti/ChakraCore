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
// eshost's lib/dependencies.js caches module/fixture source by basename
// only (rawSourceCache) and never clears it, so within one run a later test
// whose fixture shares a basename with an earlier test's fixture silently
// receives the first test's source. Upstream exports rawSource.clear() but
// nothing calls it; clear before each scenario instead. This is safe with
// the 8-agent pool: ConsoleAgent.evalScript reads the cache
// (getDependencies/rawSource.get) entirely within its synchronous prefix,
// before its first await, so no other agent can interleave between the
// clear and the reads. Cost is re-reading dependency files per scenario.
const { rawSource } = require('eshost/lib/dependencies');
const evaluate = ChakraAgent.prototype.evalScript;
ChakraAgent.prototype.evalScript = async function(...args) {
    rawSource.clear();
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
