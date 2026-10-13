//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: Silent SyntaxError thrown at runtime from a function named `code` must not satisfy a parse-phase negative (C1 wrong-phase control; its frame is space-prefixed "at code (...)", the parse frame is tab-prefixed)
flags: [onlyStrict]
negative:
  phase: parse
  type: SyntaxError
---*/
function code() { throw new SyntaxError('runtime masquerade'); }
code();
