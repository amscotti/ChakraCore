//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// W7 multi-value probe: a (func (result i32 i32)) export called from JS
// returns Array [1,2] (per ABI Q4, kept single-valued AsmJsRetType).
// Module: multivalue_return.wasm exports "get" returning (1, 2).

WebAssembly.instantiate(readbuffer('multivalue_return.wasm'), {})
.then(function(result)
{
    var instance = result.instance;
    var arr = instance.exports.get();
    if (!arr || arr.length !== 2 || arr[0] !== 1 || arr[1] !== 2) {
        print('fail');
    } else {
        print('pass');
    }
})
.catch(function(reason){ print('fail ' + reason); });
