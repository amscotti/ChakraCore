//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: A stackless runtime throw from module evaluation must not satisfy a resolution-phase negative (host marker control; the thrown-at loader shape is misleading here and only the marker catches it)
flags: [module]
negative:
  phase: resolution
  type: SyntaxError
---*/
throw { toString() { return 'SyntaxError: stackless runtime masquerade'; } };
