//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Promise continuation and completion control
flags: [onlyStrict, async]
---*/
Promise.resolve(2).then(function(value) {
    assert.sameValue(value, 2);
}).then($DONE, $DONE);
