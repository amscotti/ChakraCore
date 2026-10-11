//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Native buffer detachment control
flags: [onlyStrict]
---*/
var buffer = new ArrayBuffer(16);
$262.detachArrayBuffer(buffer);
assert.sameValue(buffer.byteLength, 0);
