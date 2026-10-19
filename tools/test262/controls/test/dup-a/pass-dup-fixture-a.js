//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Duplicate-basename fixture control A (eshost rawSource cache must not serve dup-b's fixture here)
flags: [module]
---*/
import value from './dupshared_FIXTURE.js';
assert.sameValue(value, "dup-A");
