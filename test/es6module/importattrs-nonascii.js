// -------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
// -------------------------------------------------------------------------------------------------------
import { inc, get } from "./importattrs-counter.js" with { "\u0100": "1", "\u00FF": "2" };
export { inc, get };
