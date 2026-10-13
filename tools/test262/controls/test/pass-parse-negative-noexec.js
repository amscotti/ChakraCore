//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Parse error after side-effect statements must not execute them (C1 phase control)
flags: [onlyStrict]
negative:
  phase: parse
  type: SyntaxError
---*/
print('C1-PARSE-EXECUTED');
$DONOTEVALUATE();
var = ;
