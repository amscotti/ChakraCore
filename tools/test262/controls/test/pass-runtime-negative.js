//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Expected runtime error control
flags: [onlyStrict]
negative:
  phase: runtime
  type: TypeError
---*/
throw new TypeError('expected');
