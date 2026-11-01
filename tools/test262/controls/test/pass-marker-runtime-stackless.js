//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: A stackless throw from module evaluation satisfies a runtime-phase negative (host marker control; runtime via the marker despite the thrown-at loader shape)
flags: [module]
negative:
  phase: runtime
  type: SyntaxError
---*/
throw { toString() { return 'SyntaxError: stackless runtime expected'; } };
