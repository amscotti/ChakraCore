//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function bigintDisplay() {
    var decimal = 123n;
    var negative = -45n;
    var multiLimb = 123456789012345678901234567890n;
    var zero = 0n;
    var wrapper = Object(1n);
    decimal; negative; multiLimb; zero; wrapper; /**bp:locals(1);evaluate('decimal + 7n',0);evaluate('wrapper',1);evaluate('BigInt.asUintN(8, 300n)',0);evaluate('typeof 1n',0);**/
    WScript.Echo("pass");
}
WScript.Attach(bigintDisplay);
