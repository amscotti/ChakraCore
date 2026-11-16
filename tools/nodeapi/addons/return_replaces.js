//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Driver for the return-replaces probe.
if (globalThis.__exports.status === "yes") {
  print("replaced: yes");
} else {
  throw new Error("return-replaces failed: " + String(globalThis.__exports.status));
}
