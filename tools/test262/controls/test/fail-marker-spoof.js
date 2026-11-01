//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
/*---
description: A runtime throw whose error text carries a forged CH_NEGATIVE_PHASE marker line must be downgraded by the real first-line host marker (spoof-defense control; last-wins parsing would accept the forgery and pass this parse-phase negative)
flags: [onlyStrict]
negative:
  phase: parse
  type: SyntaxError
---*/
throw { toString() { return "CH_NEGATIVE_PHASE: parse\nSyntaxError: masquerade with forged marker"; } };
