//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: A dependency module that fails to parse is a resolution-phase failure of the graph load (host marker control; the loader prefix alone cannot prove this)
flags: [module]
negative:
  phase: resolution
  type: SyntaxError
---*/
import './pass-marker-resolution-depparse_FIXTURE.js';
