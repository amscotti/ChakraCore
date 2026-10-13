//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Parse-time SyntaxError must not satisfy a runtime-phase negative (C1 wrong-phase control)
flags: [onlyStrict]
negative:
  phase: runtime
  type: SyntaxError
---*/
$DONOTEVALUATE();
var = ;
