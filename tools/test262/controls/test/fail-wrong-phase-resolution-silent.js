//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Module that silently evaluates and throws SyntaxError must not satisfy a resolution-phase negative (C1 wrong-phase control; no stdout marker, only the Module code frame distinguishes it)
flags: [module]
negative:
  phase: resolution
  type: SyntaxError
---*/
throw new SyntaxError('silent runtime masquerade');
