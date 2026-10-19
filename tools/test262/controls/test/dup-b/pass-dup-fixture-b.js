//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Duplicate-basename fixture control B (eshost rawSource cache must not serve dup-a's fixture here)
flags: [module]
---*/
import value from './dupshared_FIXTURE.js';
assert.sameValue(value, "dup-B");
