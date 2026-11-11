//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

class dummy {
    constructor() {
        return new Int16Array(4);
    }
}

var handler = {
    get: function(oTarget, sKey) {
        if (sKey.toString()=="constructor") {
            return { [Symbol.species] : dummy };
        } else {
            return 4;
        }
    },

    has: function (oTarget, sKey) {
        return Reflect.has(oTarget, sKey);
    },
};

var array = [1];
var proxy = new Proxy(array, handler);

try
{
    // Post-ES2016 (tc39 PR #2164): defining configurable index properties on a
    // TypedArray succeeds silently, so filter with a TypedArray species result
    // completes without throwing (Node-verified).
    var boundFilter = Array.prototype.filter.bind(proxy);
    boundFilter(function() { return true; });
    WScript.Echo("passed");
}
catch (e)
{
    WScript.Echo("unexpected throw: " + e);
}
