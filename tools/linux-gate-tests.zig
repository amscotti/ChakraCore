//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! Test root for the Linux gate unit tests, run by the Linux
//! `check-selftest` step (tools/build/linux.zig): the PAL-config
//! offline-render test in pal-config-linux/check.zig and the
//! gate-helper tests in build/linux.zig.
//!
//! This file lives directly under tools/ because Zig 0.17 roots a
//! module at its root file's directory and rejects imports outside
//! that directory: pal-config-linux/probes.zig imports
//! `../pal-config/probes.zig`, so a test root inside
//! pal-config-linux/ (or build/) cannot compile that graph (see
//! the header of pal-config-linux/probes.zig). Referencing the
//! files from a test block makes the test runner collect their
//! test declarations.

test {
    _ = @import("pal-config-linux/check.zig");
    _ = @import("build/linux.zig");
}
