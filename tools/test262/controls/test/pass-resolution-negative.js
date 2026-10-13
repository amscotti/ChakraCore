//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Unresolvable imported binding fails at resolution, before evaluation (C1 phase control)
flags: [module]
negative:
  phase: resolution
  type: SyntaxError
---*/
import { missing } from './pass-resolution-negative_FIXTURE.js';
print('C1-RESOLUTION-EXECUTED');
$DONOTEVALUATE();
