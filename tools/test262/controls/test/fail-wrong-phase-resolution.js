//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Module that evaluates and throws SyntaxError must not satisfy a resolution-phase negative (C1 wrong-phase control)
flags: [module]
negative:
  phase: resolution
  type: SyntaxError
---*/
print('C1-WRONG-PHASE-EXECUTED');
throw new SyntaxError('runtime masquerade');
