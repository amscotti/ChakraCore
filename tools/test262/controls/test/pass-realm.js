//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Realm isolation control
flags: [onlyStrict]
---*/
var realm = $262.createRealm();
assert.notSameValue(realm.global.Array, Array);
