//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Zig 0.17 native-only ownership example; not a Recycler object or JsRT binding.
const std = @import("std");
const Allocator = std.mem.Allocator;

const OwnedBytes = struct {
    allocator: Allocator,
    allocation: []u8,

    // Success transfers one native owner handle to the caller. The allocator's
    // implementation must remain alive at a stable address until destroy.
    fn create(allocator: Allocator, input: []const u8) !*OwnedBytes {
        const self = try allocator.create(OwnedBytes);
        errdefer allocator.destroy(self);

        const allocation = try allocator.dupe(u8, input);
        errdefer allocator.free(allocation);

        self.* = .{ .allocator = allocator, .allocation = allocation };
        return self;
    }

    // Borrowed view; neither this slice nor a copied handle transfers ownership.
    // Its lifetime ends when the single owner destroys this object.
    fn view(self: *const OwnedBytes) []const u8 {
        return self.allocation;
    }

    fn destroy(self: *OwnedBytes) void {
        const allocator = self.allocator;
        allocator.free(self.allocation); // Full allocation, not a logical subslice.
        allocator.destroy(self); // Nothing may read self afterward.
    }
};

fn exerciseOwnership(allocator: Allocator, input: []const u8) !void {
    const owned = try OwnedBytes.create(allocator, input);
    defer owned.destroy();
    try std.testing.expectEqualSlices(u8, input, owned.view());
}

const ForeignStatus = enum { ok, rejected };

// A teaching stand-in for a foreign status code. Returning the status directly
// from a non-error function would not activate errdefer.
fn publishAfterForeignStatus(allocator: Allocator, input: []const u8, status: ForeignStatus) !*OwnedBytes {
    const owned = try OwnedBytes.create(allocator, input);
    errdefer owned.destroy();
    if (status != .ok) return error.ForeignRejected;
    return owned;
}

test "owned output is independent from the input borrow" {
    var input = [_]u8{ 1, 2, 3 };
    const owned = try OwnedBytes.create(std.testing.allocator, &input);
    defer owned.destroy();
    input[0] = 99;
    try std.testing.expectEqualSlices(u8, &.{ 1, 2, 3 }, owned.view());
}

test "every constructor allocation failure rolls back native resources" {
    try std.testing.checkAllAllocationFailures(
        std.testing.allocator,
        exerciseOwnership,
        .{@as([]const u8, "owned output")},
    );
}

test "repeated owners and zero-length views have balanced release" {
    for (0..64) |_| {
        try exerciseOwnership(std.testing.allocator, "bounded repeat");
        try exerciseOwnership(std.testing.allocator, "");
    }
}

test "foreign status rejection enters rollback before ownership publication" {
    try std.testing.expectError(
        error.ForeignRejected,
        publishAfterForeignStatus(std.testing.allocator, "not published", .rejected),
    );
    const published = try publishAfterForeignStatus(std.testing.allocator, "published", .ok);
    defer published.destroy();
    try std.testing.expectEqualSlices(u8, "published", published.view());
}
