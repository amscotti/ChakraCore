//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Dynamic import from a Script - relative to the script's own path
// (HostImportModuleDynamically resolves against the referencing script;
// updated with the B1 dynamic-import host loader fix)
print("script0");
import('../mod1.js').catch(e => print ("Should not be printed"));
import('../mod2/mod2.js').catch(e => print ("Should not be printed"));
