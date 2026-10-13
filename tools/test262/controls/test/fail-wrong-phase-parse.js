//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Runtime-thrown SyntaxError must not satisfy a parse-phase negative (C1 wrong-phase control)
flags: [onlyStrict]
negative:
  phase: parse
  type: SyntaxError
---*/
print('C1-WRONG-PHASE-EXECUTED');
throw new SyntaxError('runtime masquerade');
