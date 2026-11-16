//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Driver for the hello-world addon. The host binds the addon exports to
// globalThis.__exports and installs a print() global. This driver calls
// __exports.hello() and prints the result; the build proof asserts the
// exact line "hello: world".
print("hello: " + globalThis.__exports.hello());
