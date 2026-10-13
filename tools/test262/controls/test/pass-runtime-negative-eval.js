//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: SyntaxError raised by eval at runtime satisfies a runtime-phase negative (C1 phase control)
flags: [onlyStrict]
negative:
  phase: runtime
  type: SyntaxError
---*/
eval('var = ;');
