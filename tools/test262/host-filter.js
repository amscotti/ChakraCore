//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
'use strict';

module.exports = test => {
    let reason;
    if (test.attrs.flags.module && test.scenario === 'strict mode') {
        reason = 'duplicate-module-scenario';
    } else if (test.attrs.flags.CanBlockIsFalse) {
        reason = 'blocking-host';
    } else if ((test.attrs.features || []).includes('IsHTMLDDA')) {
        reason = 'unsupported-optional-HTMLDDA-host';
    }
    if (reason) {
        console.error('TEST262_SKIP:' + JSON.stringify({ file: test.file, scenario: test.scenario, reason }));
        return false;
    }
    return true;
};
