//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Freestanding/leaf Zig 0.17 SIMD codec for ES2026 Uint8Array Base64 & Hex.
// Zero heap allocators, zero Recycler pointers, zero exceptions, zero syscalls/I/O.
// Exported symbols use hidden visibility so libChakraCore never leaks internal symbols.

const std = @import("std");

pub const Status = enum(u8) {
    ok = 0,
    syntax_error = 1,
};

pub const LastChunkHandling = enum(u8) {
    loose = 0,
    strict = 1,
    stop_before_partial = 2,
};

const hex_digits = "0123456789abcdef";
const b64_std = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const b64_url = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

comptime {
    @export(&zig_u8a_encode_hex, .{
        .name = "zig_u8a_encode_hex",
        .linkage = .strong,
        .visibility = .hidden,
    });
    @export(&zig_u8a_decode_hex, .{
        .name = "zig_u8a_decode_hex",
        .linkage = .strong,
        .visibility = .hidden,
    });
    @export(&zig_u8a_encode_base64_len, .{
        .name = "zig_u8a_encode_base64_len",
        .linkage = .strong,
        .visibility = .hidden,
    });
    @export(&zig_u8a_encode_base64, .{
        .name = "zig_u8a_encode_base64",
        .linkage = .strong,
        .visibility = .hidden,
    });
    @export(&zig_u8a_decode_base64_max_len, .{
        .name = "zig_u8a_decode_base64_max_len",
        .linkage = .strong,
        .visibility = .hidden,
    });
    @export(&zig_u8a_decode_base64, .{
        .name = "zig_u8a_decode_base64",
        .linkage = .strong,
        .visibility = .hidden,
    });
}

inline fn isAsciiWhitespace(cu: u16) bool {
    return cu == 0x0009 or cu == 0x000A or cu == 0x000C or cu == 0x000D or cu == 0x0020;
}

inline fn hexNibble(cu: u16) ?u8 {
    if (cu >= '0' and cu <= '9') return @intCast(cu - '0');
    if (cu >= 'a' and cu <= 'f') return @intCast(cu - 'a' + 10);
    if (cu >= 'A' and cu <= 'F') return @intCast(cu - 'A' + 10);
    return null;
}

inline fn decodeBase64Char(cu: u16, url_alphabet: bool) ?u8 {
    if (cu >= 'A' and cu <= 'Z') return @intCast(cu - 'A');
    if (cu >= 'a' and cu <= 'z') return @intCast(cu - 'a' + 26);
    if (cu >= '0' and cu <= '9') return @intCast(cu - '0' + 52);
    if (url_alphabet) {
        if (cu == '-') return 62;
        if (cu == '_') return 63;
    } else {
        if (cu == '+') return 62;
        if (cu == '/') return 63;
    }
    return null;
}

inline fn writeByte(dst: ?[*]u8, idx: usize, val: u8) void {
    if (dst) |d| d[idx] = val;
}

pub fn zig_u8a_encode_hex(src: ?[*]const u8, src_len: usize, dst: ?[*]u16) callconv(.c) void {
    if (src_len == 0) return;
    const s = src orelse return;
    const d = dst orelse return;

    var i: usize = 0;
    const lo_mask = [16]i32{
        0, ~@as(i32, 0), 1, ~@as(i32, 1), 2, ~@as(i32, 2), 3, ~@as(i32, 3),
        4, ~@as(i32, 4), 5, ~@as(i32, 5), 6, ~@as(i32, 6), 7, ~@as(i32, 7),
    };
    const hi_mask = [16]i32{
        8,  ~@as(i32, 8),  9,  ~@as(i32, 9),  10, ~@as(i32, 10), 11, ~@as(i32, 11),
        12, ~@as(i32, 12), 13, ~@as(i32, 13), 14, ~@as(i32, 14), 15, ~@as(i32, 15),
    };
    const splat_4: @Vector(16, u3) = @splat(4);
    const splat_0f: @Vector(16, u8) = @splat(0x0F);
    const splat_9: @Vector(16, u8) = @splat(9);
    const splat_zero: @Vector(16, u8) = @splat('0');
    const splat_alpha: @Vector(16, u8) = @splat('a' - 10);

    while (i + 16 <= src_len) {
        const v: @Vector(16, u8) = @as(*align(1) const @Vector(16, u8), @ptrCast(s + i)).*;
        const hi: @Vector(16, u8) = v >> splat_4;
        const lo: @Vector(16, u8) = v & splat_0f;
        const hi_c: @Vector(16, u8) = @select(u8, hi > splat_9, hi +% splat_alpha, hi +% splat_zero);
        const lo_c: @Vector(16, u8) = @select(u8, lo > splat_9, lo +% splat_alpha, lo +% splat_zero);

        const out0: @Vector(16, u16) = @shuffle(u8, hi_c, lo_c, lo_mask);
        const out1: @Vector(16, u16) = @shuffle(u8, hi_c, lo_c, hi_mask);
        @as(*align(1) @Vector(16, u16), @ptrCast(d + 2 * i)).* = out0;
        @as(*align(1) @Vector(16, u16), @ptrCast(d + 2 * i + 16)).* = out1;
        i += 16;
    }

    while (i < src_len) : (i += 1) {
        const b = s[i];
        d[2 * i] = hex_digits[b >> 4];
        d[2 * i + 1] = hex_digits[b & 0x0F];
    }
}

pub fn zig_u8a_decode_hex(
    src: ?[*]const u16,
    src_len: usize,
    dst: ?[*]u8,
    dst_cap: usize,
    out_read: *usize,
    out_written: *usize,
) callconv(.c) u8 {
    out_read.* = 0;
    out_written.* = 0;

    // ES2026 FromHex checks odd length before maxLength or reading digits.
    if ((src_len & 1) != 0) {
        return @intFromEnum(Status.syntax_error);
    }
    if (src_len == 0 or dst_cap == 0) {
        return @intFromEnum(Status.ok);
    }

    const s = src orelse return @intFromEnum(Status.syntax_error);
    const max_pairs = @min(src_len / 2, dst_cap);
    var pair_idx: usize = 0;

    const even_mask = [16]i32{
        0, 2, 4, 6, 8, 10, 12, 14,
        ~@as(i32, 0), ~@as(i32, 2), ~@as(i32, 4), ~@as(i32, 6), ~@as(i32, 8), ~@as(i32, 10), ~@as(i32, 12), ~@as(i32, 14),
    };
    const odd_mask = [16]i32{
        1, 3, 5, 7, 9, 11, 13, 15,
        ~@as(i32, 1), ~@as(i32, 3), ~@as(i32, 5), ~@as(i32, 7), ~@as(i32, 9), ~@as(i32, 11), ~@as(i32, 13), ~@as(i32, 15),
    };
    const splat_zero: @Vector(16, u8) = @splat('0');
    const splat_9: @Vector(16, u8) = @splat(9);
    const splat_20: @Vector(16, u8) = @splat(0x20);
    const splat_a: @Vector(16, u8) = @splat('a');
    const splat_5: @Vector(16, u8) = @splat(5);
    const splat_10: @Vector(16, u8) = @splat(10);
    const splat_4: @Vector(16, u3) = @splat(4);

    while (pair_idx + 16 <= max_pairs) {
        const v0: @Vector(16, u16) = @as(*align(1) const @Vector(16, u16), @ptrCast(s + pair_idx * 2)).*;
        const v1: @Vector(16, u16) = @as(*align(1) const @Vector(16, u16), @ptrCast(s + pair_idx * 2 + 16)).*;
        if (@reduce(.Or, v0 | v1) >= 256) break;

        const b0: @Vector(16, u8) = @truncate(v0);
        const b1: @Vector(16, u8) = @truncate(v1);
        const hi_c = @shuffle(u8, b0, b1, even_mask);
        const lo_c = @shuffle(u8, b0, b1, odd_mask);

        const hi_d = hi_c -% splat_zero;
        const lo_d = lo_c -% splat_zero;
        const hi_is_d = hi_d <= splat_9;
        const lo_is_d = lo_d <= splat_9;

        const hi_a = (hi_c | splat_20) -% splat_a;
        const lo_a = (lo_c | splat_20) -% splat_a;
        const hi_is_a = hi_a <= splat_5;
        const lo_is_a = lo_a <= splat_5;

        const valid = (hi_is_d | hi_is_a) & (lo_is_d | lo_is_a);
        if (!@reduce(.And, valid)) break;

        const hi_nib = @select(u8, hi_is_d, hi_d, hi_a +% splat_10);
        const lo_nib = @select(u8, lo_is_d, lo_d, lo_a +% splat_10);
        const decoded: @Vector(16, u8) = (hi_nib << splat_4) | lo_nib;
        if (dst) |d| {
            @as(*align(1) @Vector(16, u8), @ptrCast(d + pair_idx)).* = decoded;
        }
        pair_idx += 16;
    }

    while (pair_idx < max_pairs) : (pair_idx += 1) {
        const n0 = hexNibble(s[pair_idx * 2]) orelse {
            out_read.* = pair_idx * 2;
            out_written.* = pair_idx;
            return @intFromEnum(Status.syntax_error);
        };
        const n1 = hexNibble(s[pair_idx * 2 + 1]) orelse {
            out_read.* = pair_idx * 2;
            out_written.* = pair_idx;
            return @intFromEnum(Status.syntax_error);
        };
        writeByte(dst, pair_idx, (n0 << 4) | n1);
    }

    out_read.* = pair_idx * 2;
    out_written.* = pair_idx;
    return @intFromEnum(Status.ok);
}

pub fn zig_u8a_encode_base64_len(src_len: usize, omit_padding: u8) callconv(.c) usize {
    const q = src_len / 3;
    const rem = src_len % 3;
    const tail: usize = if (rem == 0) 0 else if (omit_padding != 0) rem + 1 else 4;
    const q4 = std.math.mul(usize, q, 4) catch return std.math.maxInt(usize);
    return std.math.add(usize, q4, tail) catch return std.math.maxInt(usize);
}

pub fn zig_u8a_encode_base64(
    src: ?[*]const u8,
    src_len: usize,
    dst: ?[*]u16,
    url_alphabet: u8,
    omit_padding: u8,
) callconv(.c) usize {
    if (src_len == 0) return 0;
    const s = src orelse return 0;
    const d = dst orelse return 0;

    const alpha = if (url_alphabet != 0) b64_url else b64_std;
    const c62: u8 = if (url_alphabet != 0) '-' else '+';
    const c63: u8 = if (url_alphabet != 0) '_' else '/';

    const mask_a_idx = [16]i32{ 0, 0, 1, 2, 3, 3, 4, 5, 6, 6, 7, 8, 9, 9, 10, 11 };
    const mask_b_idx = [16]i32{ 0, 1, 2, 2, 3, 4, 5, 5, 6, 7, 8, 8, 9, 10, 11, 11 };
    const mask_a = @Vector(16, u8){ 0x00, 0x03, 0x0F, 0x3F, 0x00, 0x03, 0x0F, 0x3F, 0x00, 0x03, 0x0F, 0x3F, 0x00, 0x03, 0x0F, 0x3F };
    const mask_b = @Vector(16, u8){ 0x3F, 0x0F, 0x03, 0x00, 0x3F, 0x0F, 0x03, 0x00, 0x3F, 0x0F, 0x03, 0x00, 0x3F, 0x0F, 0x03, 0x00 };
    const shift_a = @Vector(16, u3){ 0, 4, 2, 0, 0, 4, 2, 0, 0, 4, 2, 0, 0, 4, 2, 0 };
    const shift_b = @Vector(16, u3){ 2, 4, 6, 0, 2, 4, 6, 0, 2, 4, 6, 0, 2, 4, 6, 0 };

    const splat_26: @Vector(16, u8) = @splat(26);
    const splat_52: @Vector(16, u8) = @splat(52);
    const splat_62: @Vector(16, u8) = @splat(62);
    const splat_63: @Vector(16, u8) = @splat(63);
    const splat_upper: @Vector(16, u8) = @splat('A');
    const splat_lower: @Vector(16, u8) = @splat('a' - 26);
    const splat_digit: @Vector(16, u8) = @splat(@as(u8, 256 - 52 + '0'));
    const splat_c62: @Vector(16, u8) = @splat(c62);
    const splat_c63: @Vector(16, u8) = @splat(c63);

    var i: usize = 0;
    var o: usize = 0;

    // Require 16 readable bytes so the 16-byte load never reads past src_len.
    while (i + 16 <= src_len) {
        const v: @Vector(16, u8) = @as(*align(1) const @Vector(16, u8), @ptrCast(s + i)).*;
        const a = @shuffle(u8, v, undefined, mask_a_idx);
        const b = @shuffle(u8, v, undefined, mask_b_idx);
        const sextets: @Vector(16, u8) = ((a & mask_a) << shift_a) | ((b >> shift_b) & mask_b);

        const is_upper = sextets < splat_26;
        const is_digit = (sextets >= splat_52) & (sextets < splat_62);
        var ascii = @select(u8, is_upper, sextets +% splat_upper, sextets +% splat_lower);
        ascii = @select(u8, is_digit, sextets +% splat_digit, ascii);
        ascii = @select(u8, sextets == splat_62, splat_c62, ascii);
        ascii = @select(u8, sextets == splat_63, splat_c63, ascii);

        const out16: @Vector(16, u16) = ascii;
        @as(*align(1) @Vector(16, u16), @ptrCast(d + o)).* = out16;
        i += 12;
        o += 16;
    }

    while (i + 3 <= src_len) {
        const b0 = s[i];
        const b1 = s[i + 1];
        const b2 = s[i + 2];
        d[o] = alpha[b0 >> 2];
        d[o + 1] = alpha[((b0 & 0x03) << 4) | (b1 >> 4)];
        d[o + 2] = alpha[((b1 & 0x0F) << 2) | (b2 >> 6)];
        d[o + 3] = alpha[b2 & 0x3F];
        i += 3;
        o += 4;
    }

    const rem = src_len - i;
    if (rem == 1) {
        const b0 = s[i];
        d[o] = alpha[b0 >> 2];
        d[o + 1] = alpha[(b0 & 0x03) << 4];
        o += 2;
        if (omit_padding == 0) {
            d[o] = '=';
            d[o + 1] = '=';
            o += 2;
        }
    } else if (rem == 2) {
        const b0 = s[i];
        const b1 = s[i + 1];
        d[o] = alpha[b0 >> 2];
        d[o + 1] = alpha[((b0 & 0x03) << 4) | (b1 >> 4)];
        d[o + 2] = alpha[(b1 & 0x0F) << 2];
        o += 3;
        if (omit_padding == 0) {
            d[o] = '=';
            o += 1;
        }
    }

    return o;
}

pub fn zig_u8a_decode_base64_max_len(src_len: usize) callconv(.c) usize {
    const q = src_len / 4;
    const rem = src_len % 4;
    const tail: usize = if (rem > 0) rem - 1 else 0;
    return q * 3 + tail;
}

inline fn tryDecodeBase64Simd16(
    s: [*]const u16,
    index: usize,
    dst: ?[*]u8,
    written: usize,
    url_alphabet: bool,
) bool {
    const v16: @Vector(16, u16) = @as(*align(1) const @Vector(16, u16), @ptrCast(s + index)).*;
    if (@reduce(.Or, v16) >= 256) return false;
    const v: @Vector(16, u8) = @truncate(v16);

    const splat_A: @Vector(16, u8) = @splat('A');
    const splat_a: @Vector(16, u8) = @splat('a');
    const splat_0: @Vector(16, u8) = @splat('0');
    const splat_25: @Vector(16, u8) = @splat(25);
    const splat_9: @Vector(16, u8) = @splat(9);
    const splat_26: @Vector(16, u8) = @splat(26);
    const splat_52: @Vector(16, u8) = @splat(52);
    const c62: u8 = if (url_alphabet) '-' else '+';
    const c63: u8 = if (url_alphabet) '_' else '/';
    const splat_c62: @Vector(16, u8) = @splat(c62);
    const splat_c63: @Vector(16, u8) = @splat(c63);

    const d_upper = v -% splat_A;
    const d_lower = v -% splat_a;
    const d_digit = v -% splat_0;
    const is_upper = d_upper <= splat_25;
    const is_lower = d_lower <= splat_25;
    const is_digit = d_digit <= splat_9;
    const is_62 = v == splat_c62;
    const is_63 = v == splat_c63;

    const valid = is_upper | is_lower | is_digit | is_62 | is_63;
    if (!@reduce(.And, valid)) return false;

    var vals = @select(u8, is_upper, d_upper, d_lower +% splat_26);
    vals = @select(u8, is_digit, d_digit +% splat_52, vals);
    vals = @select(u8, is_62, @as(@Vector(16, u8), @splat(62)), vals);
    vals = @select(u8, is_63, @as(@Vector(16, u8), @splat(63)), vals);

    const a_idx = [16]i32{ 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, 0, 0, 0, 0 };
    const b_idx = [16]i32{ 1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15, 0, 0, 0, 0 };
    const shift_l = @Vector(16, u3){ 2, 4, 6, 2, 4, 6, 2, 4, 6, 2, 4, 6, 0, 0, 0, 0 };
    const shift_r = @Vector(16, u3){ 4, 2, 0, 4, 2, 0, 4, 2, 0, 4, 2, 0, 0, 0, 0, 0 };

    const va = @shuffle(u8, vals, undefined, a_idx);
    const vb = @shuffle(u8, vals, undefined, b_idx);
    const packed_vec: @Vector(16, u8) = (va << shift_l) | (vb >> shift_r);
    if (dst) |d| {
        const bytes: [16]u8 = packed_vec;
        @as(*[12]u8, @ptrCast(d + written)).* = bytes[0..12].*;
    }
    return true;
}

pub fn zig_u8a_decode_base64(
    src: ?[*]const u16,
    src_len: usize,
    dst: ?[*]u8,
    dst_cap: usize,
    url_alphabet: u8,
    last_chunk_mode: u8,
    out_read: *usize,
    out_written: *usize,
) callconv(.c) u8 {
    out_read.* = 0;
    out_written.* = 0;

    const mode: LastChunkHandling = switch (last_chunk_mode) {
        0 => .loose,
        1 => .strict,
        2 => .stop_before_partial,
        else => return @intFromEnum(Status.syntax_error),
    };

    if (dst_cap == 0 or src_len == 0) {
        return @intFromEnum(Status.ok);
    }

    const s = src orelse return @intFromEnum(Status.syntax_error);
    const is_url = url_alphabet != 0;

    var index: usize = 0;
    var read: usize = 0;
    var written: usize = 0;
    var chunk: [4]u8 = undefined;
    var chunk_len: u8 = 0;

    while (true) {
        if (chunk_len == 0 and index + 16 <= src_len and dst_cap - written >= 12) {
            if (tryDecodeBase64Simd16(s, index, dst, written, is_url)) {
                index += 16;
                read = index;
                written += 12;
                if (written == dst_cap) {
                    out_read.* = read;
                    out_written.* = written;
                    return @intFromEnum(Status.ok);
                }
                continue;
            }
        }

        while (index < src_len and isAsciiWhitespace(s[index])) : (index += 1) {}
        if (index == src_len) {
            if (chunk_len > 0) {
                switch (mode) {
                    .stop_before_partial => {
                        out_read.* = read;
                        out_written.* = written;
                        return @intFromEnum(Status.ok);
                    },
                    .strict => {
                        out_read.* = read;
                        out_written.* = written;
                        return @intFromEnum(Status.syntax_error);
                    },
                    .loose => {
                        if (chunk_len == 1) {
                            out_read.* = read;
                            out_written.* = written;
                            return @intFromEnum(Status.syntax_error);
                        }
                        if (chunk_len == 2) {
                            writeByte(dst, written, (chunk[0] << 2) | (chunk[1] >> 4));
                            written += 1;
                        } else {
                            writeByte(dst, written, (chunk[0] << 2) | (chunk[1] >> 4));
                            writeByte(dst, written + 1, ((chunk[1] & 0x0F) << 4) | (chunk[2] >> 2));
                            written += 2;
                        }
                    },
                }
            }
            out_read.* = src_len;
            out_written.* = written;
            return @intFromEnum(Status.ok);
        }

        const cu = s[index];
        index += 1;

        if (cu == '=') {
            if (chunk_len < 2) {
                out_read.* = read;
                out_written.* = written;
                return @intFromEnum(Status.syntax_error);
            }
            while (index < src_len and isAsciiWhitespace(s[index])) : (index += 1) {}
            if (chunk_len == 2) {
                if (index == src_len) {
                    out_read.* = read;
                    out_written.* = written;
                    return @intFromEnum(if (mode == .stop_before_partial) Status.ok else Status.syntax_error);
                }
                if (s[index] != '=') {
                    out_read.* = read;
                    out_written.* = written;
                    return @intFromEnum(Status.syntax_error);
                }
                index += 1;
                while (index < src_len and isAsciiWhitespace(s[index])) : (index += 1) {}
            }
            if (index < src_len) {
                out_read.* = read;
                out_written.* = written;
                return @intFromEnum(Status.syntax_error);
            }
            if (chunk_len == 2) {
                if (mode == .strict and (chunk[1] & 0x0F) != 0) {
                    out_read.* = read;
                    out_written.* = written;
                    return @intFromEnum(Status.syntax_error);
                }
                writeByte(dst, written, (chunk[0] << 2) | (chunk[1] >> 4));
                written += 1;
            } else {
                if (mode == .strict and (chunk[2] & 0x03) != 0) {
                    out_read.* = read;
                    out_written.* = written;
                    return @intFromEnum(Status.syntax_error);
                }
                writeByte(dst, written, (chunk[0] << 2) | (chunk[1] >> 4));
                writeByte(dst, written + 1, ((chunk[1] & 0x0F) << 4) | (chunk[2] >> 2));
                written += 2;
            }
            out_read.* = src_len;
            out_written.* = written;
            return @intFromEnum(Status.ok);
        }

        const val = decodeBase64Char(cu, is_url) orelse {
            out_read.* = read;
            out_written.* = written;
            return @intFromEnum(Status.syntax_error);
        };

        const remaining = dst_cap - written;
        if ((remaining == 1 and chunk_len == 2) or (remaining == 2 and chunk_len == 3)) {
            out_read.* = read;
            out_written.* = written;
            return @intFromEnum(Status.ok);
        }

        chunk[chunk_len] = val;
        chunk_len += 1;
        if (chunk_len == 4) {
            read = index;
            writeByte(dst, written, (chunk[0] << 2) | (chunk[1] >> 4));
            writeByte(dst, written + 1, ((chunk[1] & 0x0F) << 4) | (chunk[2] >> 2));
            writeByte(dst, written + 2, ((chunk[2] & 0x03) << 6) | chunk[3]);
            written += 3;
            chunk_len = 0;
            if (written == dst_cap) {
                out_read.* = read;
                out_written.* = written;
                return @intFromEnum(Status.ok);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Self-contained unit tests (`zig test lib/ZigCodec/u8a_codec.zig`)
// ---------------------------------------------------------------------------

fn expectEncodeHex(bytes: []const u8, want_ascii: []const u8) !void {
    const allocator = std.testing.allocator;
    const dst = try allocator.alloc(u16, bytes.len * 2);
    defer allocator.free(dst);
    zig_u8a_encode_hex(if (bytes.len == 0) null else bytes.ptr, bytes.len, if (dst.len == 0) null else dst.ptr);
    try std.testing.expectEqual(want_ascii.len, dst.len);
    for (want_ascii, 0..) |ch, idx| {
        try std.testing.expectEqual(@as(u16, ch), dst[idx]);
    }
}

fn toUtf16(allocator: std.mem.Allocator, ascii: []const u8) ![]u16 {
    const out = try allocator.alloc(u16, ascii.len);
    for (ascii, 0..) |c, i| out[i] = c;
    return out;
}

fn expectDecodeBase64(
    src16: []const u16,
    dst_cap: usize,
    url_alphabet: bool,
    mode: LastChunkHandling,
    want_status: Status,
    want_read: usize,
    want_bytes: []const u8,
) !void {
    const allocator = std.testing.allocator;
    const dst = try allocator.alloc(u8, dst_cap);
    defer allocator.free(dst);
    @memset(dst, 0xFF);
    var out_read: usize = 999;
    var out_written: usize = 999;
    const st = zig_u8a_decode_base64(
        if (src16.len == 0) null else src16.ptr,
        src_len_or: {
            break :src_len_or src16.len;
        },
        if (dst_cap == 0) null else dst.ptr,
        dst_cap,
        @intFromBool(url_alphabet),
        @intFromEnum(mode),
        &out_read,
        &out_written,
    );
    try std.testing.expectEqual(@intFromEnum(want_status), st);
    try std.testing.expectEqual(want_read, out_read);
    try std.testing.expectEqual(want_bytes.len, out_written);
    try std.testing.expectEqualSlices(u8, want_bytes, dst[0..out_written]);
    for (dst[out_written..]) |b| {
        try std.testing.expectEqual(@as(u8, 0xFF), b);
    }
}

test "RFC 4648 hex encode and decode vectors" {
    try expectEncodeHex("", "");
    try expectEncodeHex("f", "66");
    try expectEncodeHex("fo", "666f");
    try expectEncodeHex("foo", "666f6f");
    try expectEncodeHex("foob", "666f6f62");
    try expectEncodeHex("fooba", "666f6f6261");
    try expectEncodeHex("foobar", "666f6f626172");

    const all_bytes = [_]u8{ 0x00, 0x01, 0x09, 0x0a, 0x0f, 0x10, 0x7f, 0x80, 0xfe, 0xff };
    try expectEncodeHex(&all_bytes, "0001090a0f107f80feff");
}

test "hex decode strict validation, odd length, writes-up-to-error, and maxLength" {
    const alloc = std.testing.allocator;

    // Odd length throws SyntaxError with 0 written even when dst_cap == 0 or > 0
    {
        const s = try toUtf16(alloc, "aaa");
        defer alloc.free(s);
        var dst = [_]u8{ 0xFF, 0xFF, 0xFF };
        var r: usize = 99;
        var w: usize = 99;
        const st = zig_u8a_decode_hex(s.ptr, s.len, &dst, dst.len, &r, &w);
        try std.testing.expectEqual(@intFromEnum(Status.syntax_error), st);
        try std.testing.expectEqual(@as(usize, 0), r);
        try std.testing.expectEqual(@as(usize, 0), w);
        try std.testing.expectEqualSlices(u8, &.{ 0xFF, 0xFF, 0xFF }, &dst);
    }
    {
        const s = try toUtf16(alloc, "1");
        defer alloc.free(s);
        var r: usize = 99;
        var w: usize = 99;
        const st = zig_u8a_decode_hex(s.ptr, s.len, null, 0, &r, &w);
        try std.testing.expectEqual(@intFromEnum(Status.syntax_error), st);
        try std.testing.expectEqual(@as(usize, 0), r);
        try std.testing.expectEqual(@as(usize, 0), w);
    }

    // Even length with whitespace or illegal char writes valid prefix pairs before error
    for ([_][]const u8{ "aaa ", "aaag", "aa\ta", "aa\na", "aa\x0ca", "aa\x0da" }) |bad| {
        const s = try toUtf16(alloc, bad);
        defer alloc.free(s);
        var dst = [_]u8{ 0xFF, 0xFF, 0xFF };
        var r: usize = 99;
        var w: usize = 99;
        const st = zig_u8a_decode_hex(s.ptr, s.len, &dst, dst.len, &r, &w);
        try std.testing.expectEqual(@intFromEnum(Status.syntax_error), st);
        try std.testing.expectEqual(@as(usize, 2), r);
        try std.testing.expectEqual(@as(usize, 1), w);
        try std.testing.expectEqualSlices(u8, &.{ 0xAA, 0xFF, 0xFF }, &dst);
    }

    // Non-ASCII and surrogate code units rejected even inside SIMD-width inputs
    {
        var wide: [36]u16 = undefined;
        for (&wide) |*c| c.* = 'a';
        wide[34] = 0xD800; // high surrogate in 2nd SIMD or tail chunk
        var dst: [18]u8 = @splat(0xFF);
        var r: usize = 0;
        var w: usize = 0;
        const st = zig_u8a_decode_hex(&wide, wide.len, &dst, dst.len, &r, &w);
        try std.testing.expectEqual(@intFromEnum(Status.syntax_error), st);
        try std.testing.expectEqual(@as(usize, 34), r);
        try std.testing.expectEqual(@as(usize, 17), w);
        for (dst[0..17]) |b| try std.testing.expectEqual(@as(u8, 0xAA), b);
        try std.testing.expectEqual(@as(u8, 0xFF), dst[17]);
    }

    // Stops cleanly at dst_cap without inspecting trailing garbage
    {
        const s = try toUtf16(alloc, "aabbzz");
        defer alloc.free(s);
        var dst = [_]u8{ 0xFF, 0xFF };
        var r: usize = 0;
        var w: usize = 0;
        const st = zig_u8a_decode_hex(s.ptr, s.len, &dst, 2, &r, &w);
        try std.testing.expectEqual(@intFromEnum(Status.ok), st);
        try std.testing.expectEqual(@as(usize, 4), r);
        try std.testing.expectEqual(@as(usize, 2), w);
        try std.testing.expectEqualSlices(u8, &.{ 0xAA, 0xBB }, &dst);
    }
}

test "RFC 4648 base64 and base64url encode with/without omit_padding" {
    const alloc = std.testing.allocator;
    const Case = struct { in: []const u8, std_pad: []const u8, std_nopad: []const u8, url_pad: []const u8, url_nopad: []const u8 };
    const cases = [_]Case{
        .{ .in = "", .std_pad = "", .std_nopad = "", .url_pad = "", .url_nopad = "" },
        .{ .in = "f", .std_pad = "Zg==", .std_nopad = "Zg", .url_pad = "Zg==", .url_nopad = "Zg" },
        .{ .in = "fo", .std_pad = "Zm8=", .std_nopad = "Zm8", .url_pad = "Zm8=", .url_nopad = "Zm8" },
        .{ .in = "foo", .std_pad = "Zm9v", .std_nopad = "Zm9v", .url_pad = "Zm9v", .url_nopad = "Zm9v" },
        .{ .in = "foob", .std_pad = "Zm9vYg==", .std_nopad = "Zm9vYg", .url_pad = "Zm9vYg==", .url_nopad = "Zm9vYg" },
        .{ .in = "fooba", .std_pad = "Zm9vYmE=", .std_nopad = "Zm9vYmE", .url_pad = "Zm9vYmE=", .url_nopad = "Zm9vYmE" },
        .{ .in = "foobar", .std_pad = "Zm9vYmFy", .std_nopad = "Zm9vYmFy", .url_pad = "Zm9vYmFy", .url_nopad = "Zm9vYmFy" },
        .{ .in = &.{ 199, 239, 242 }, .std_pad = "x+/y", .std_nopad = "x+/y", .url_pad = "x-_y", .url_nopad = "x-_y" },
        .{ .in = &.{255}, .std_pad = "/w==", .std_nopad = "/w", .url_pad = "_w==", .url_nopad = "_w" },
    };
    for (cases) |c| {
        inline for (.{
            .{ false, false, c.std_pad },
            .{ false, true, c.std_nopad },
            .{ true, false, c.url_pad },
            .{ true, true, c.url_nopad },
        }) |cfg| {
            const want_len = zig_u8a_encode_base64_len(c.in.len, @intFromBool(cfg[1]));
            try std.testing.expectEqual(cfg[2].len, want_len);
            const buf = try alloc.alloc(u16, want_len);
            defer alloc.free(buf);
            const got_len = zig_u8a_encode_base64(
                if (c.in.len == 0) null else c.in.ptr,
                c.in.len,
                if (want_len == 0) null else buf.ptr,
                @intFromBool(cfg[0]),
                @intFromBool(cfg[1]),
            );
            try std.testing.expectEqual(want_len, got_len);
            for (cfg[2], 0..) |ch, idx| {
                try std.testing.expectEqual(@as(u16, ch), buf[idx]);
            }
        }
    }
}

test "base64 lastChunkHandling modes (loose, strict, stop-before-partial) and non-zero extra bits" {
    const alloc = std.testing.allocator;

    // Missing padding: "ZXhhZg" -> loose ok [101,120,97,102], stop-before-partial [101,120,97], strict error
    {
        const s = try toUtf16(alloc, "ZXhhZg");
        defer alloc.free(s);
        try expectDecodeBase64(s, 8, false, .loose, .ok, 6, "exaf");
        try expectDecodeBase64(s, 8, false, .stop_before_partial, .ok, 4, "exa");
        try expectDecodeBase64(s, 8, false, .strict, .syntax_error, 4, "exa");
    }

    // Non-zero padding bits with padding: "ZXhhZh==" -> loose/stop ok, strict error
    {
        const s = try toUtf16(alloc, "ZXhhZh==");
        defer alloc.free(s);
        try expectDecodeBase64(s, 8, false, .loose, .ok, 8, "exaf");
        try expectDecodeBase64(s, 8, false, .stop_before_partial, .ok, 8, "exaf");
        try expectDecodeBase64(s, 8, false, .strict, .syntax_error, 4, "exa");
    }

    // Partial padding: "ZXhhZg=" -> loose/strict error, stop-before-partial ok [101,120,97]
    {
        const s = try toUtf16(alloc, "ZXhhZg=");
        defer alloc.free(s);
        try expectDecodeBase64(s, 8, false, .loose, .syntax_error, 4, "exa");
        try expectDecodeBase64(s, 8, false, .strict, .syntax_error, 4, "exa");
        try expectDecodeBase64(s, 8, false, .stop_before_partial, .ok, 4, "exa");
    }

    // Excess padding or incomplete single char
    for ([_][]const u8{ "ZXhhZg===", "=", "A=", "AAA==", "AAAA=" }) |bad| {
        const s = try toUtf16(alloc, bad);
        defer alloc.free(s);
        for ([_]LastChunkHandling{ .loose, .strict, .stop_before_partial }) |m| {
            var dst: [8]u8 = @splat(0xFF);
            var r: usize = 0;
            var w: usize = 0;
            const st = zig_u8a_decode_base64(s.ptr, s.len, &dst, dst.len, 0, @intFromEnum(m), &r, &w);
            try std.testing.expectEqual(@intFromEnum(Status.syntax_error), st);
        }
    }

    // Single trailing char "ABCDA": error in loose/strict, stop-before-partial writes 3 bytes
    {
        const s = try toUtf16(alloc, "ABCDA");
        defer alloc.free(s);
        try expectDecodeBase64(s, 8, false, .loose, .syntax_error, 4, &.{ 0, 16, 131 });
        try expectDecodeBase64(s, 8, false, .strict, .syntax_error, 4, &.{ 0, 16, 131 });
        try expectDecodeBase64(s, 8, false, .stop_before_partial, .ok, 4, &.{ 0, 16, 131 });
    }
}

test "base64 maxLength lookahead stops, trailing-garbage, and writes-up-to-error" {
    const alloc = std.testing.allocator;

    // maxLength == 0 ignores garbage
    {
        const s = try toUtf16(alloc, "#");
        defer alloc.free(s);
        try expectDecodeBase64(s, 0, false, .strict, .ok, 0, "");
    }

    // remaining == 1 or 2 never consumes a 4-char non-padded chunk
    {
        const s = try toUtf16(alloc, "Zm9vYmFy");
        defer alloc.free(s);
        try expectDecodeBase64(s, 4, false, .loose, .ok, 4, "foo");
        try expectDecodeBase64(s, 5, false, .loose, .ok, 4, "foo");
        try expectDecodeBase64(s, 6, false, .loose, .ok, 8, "foobar");
    }

    // remaining == 1 accepts 2-char padded chunk "Zg==" -> 1 byte, stops before 3-char "Zm8="
    {
        const s1 = try toUtf16(alloc, "Zm9vZg==");
        defer alloc.free(s1);
        try expectDecodeBase64(s1, 4, false, .strict, .ok, 8, "foof");

        const s2 = try toUtf16(alloc, "Zm9vZm8=");
        defer alloc.free(s2);
        try expectDecodeBase64(s2, 4, false, .strict, .ok, 4, "foo");
        try expectDecodeBase64(s2, 5, false, .strict, .ok, 8, "foofo");
    }

    // trailing garbage after complete chunk when dst_cap is exhausted is ignored
    {
        const s = try toUtf16(alloc, "aaaa#");
        defer alloc.free(s);
        try expectDecodeBase64(s, 3, false, .strict, .ok, 4, &.{ 0x69, 0xa6, 0x9a });
    }
}

test "base64 whitespace vs non-ASCII-whitespace and non-ASCII/surrogate rejection" {
    const alloc = std.testing.allocator;

    // Valid ASCII whitespace: \t, \n, \f, \r, ' '
    const valid_ws = [_][]const u8{ "Z g==", "Z\tg==", "Z\x0Ag==", "Z\x0Cg==", "Z\x0Dg==", " Z\t\n\x0C\x0Dg = = " };
    for (valid_ws) |ws| {
        const s = try toUtf16(alloc, ws);
        defer alloc.free(s);
        try expectDecodeBase64(s, 4, false, .strict, .ok, ws.len, "f");
    }

    // Vertical tab \v (0x0B) and NBSP (0x00A0) and surrogates (0xD800) must be rejected
    const bad_u16 = [_][]const u16{
        &.{ 'Z', 0x000B, 'g', '=', '=' },
        &.{ 'Z', 0x00A0, 'g', '=', '=' },
        &.{ 'Z', 0xD800, 'g', '=', '=' },
        &.{ 'Z', 0x2009, 'g', '=', '=' },
        &.{ 'Z', 0x2028, 'g', '=', '=' },
    };
    for (bad_u16) |s| {
        try expectDecodeBase64(s, 4, false, .loose, .syntax_error, 0, "");
    }
}

test "SIMD boundary equivalence across lengths 0..512 for hex and base64" {
    const alloc = std.testing.allocator;
    var prng = std.Random.DefaultPrng.init(0x5EED_1234_ABCD_EF01);
    const rand = prng.random();

    for (0..260) |len| {
        const raw = try alloc.alloc(u8, len);
        defer alloc.free(raw);
        rand.bytes(raw);

        // Hex roundtrip (with upper/lower case mix on decode)
        const hex_buf = try alloc.alloc(u16, len * 2);
        defer alloc.free(hex_buf);
        zig_u8a_encode_hex(if (len == 0) null else raw.ptr, len, if (len == 0) null else hex_buf.ptr);
        for (raw, 0..) |b, i| {
            try std.testing.expectEqual(@as(u16, hex_digits[b >> 4]), hex_buf[2 * i]);
            try std.testing.expectEqual(@as(u16, hex_digits[b & 0x0F]), hex_buf[2 * i + 1]);
            // Flip some hex letters to uppercase to exercise both cases in SIMD decode
            if ((i & 1) == 1 and hex_buf[2 * i] >= 'a') hex_buf[2 * i] -= 32;
        }
        const hex_out = try alloc.alloc(u8, len);
        defer alloc.free(hex_out);
        var hr: usize = 0;
        var hw: usize = 0;
        const hst = zig_u8a_decode_hex(
            if (len == 0) null else hex_buf.ptr,
            hex_buf.len,
            if (len == 0) null else hex_out.ptr,
            len,
            &hr,
            &hw,
        );
        try std.testing.expectEqual(@intFromEnum(Status.ok), hst);
        try std.testing.expectEqual(hex_buf.len, hr);
        try std.testing.expectEqual(len, hw);
        try std.testing.expectEqualSlices(u8, raw, hex_out);

        // Base64 & Base64URL roundtrip (padded and unpadded)
        for ([_]bool{ false, true }) |is_url| {
            for ([_]bool{ false, true }) |omit_pad| {
                const enc_len = zig_u8a_encode_base64_len(len, @intFromBool(omit_pad));
                const b64_buf = try alloc.alloc(u16, enc_len);
                defer alloc.free(b64_buf);
                const wrote_len = zig_u8a_encode_base64(
                    if (len == 0) null else raw.ptr,
                    len,
                    if (enc_len == 0) null else b64_buf.ptr,
                    @intFromBool(is_url),
                    @intFromBool(omit_pad),
                );
                try std.testing.expectEqual(enc_len, wrote_len);

                // Scalar reference check on encoded chars
                const alpha = if (is_url) b64_url else b64_std;
                var si: usize = 0;
                var so: usize = 0;
                while (si + 3 <= len) : ({
                    si += 3;
                    so += 4;
                }) {
                    try std.testing.expectEqual(@as(u16, alpha[raw[si] >> 2]), b64_buf[so]);
                    try std.testing.expectEqual(@as(u16, alpha[((raw[si] & 3) << 4) | (raw[si + 1] >> 4)]), b64_buf[so + 1]);
                    try std.testing.expectEqual(@as(u16, alpha[((raw[si + 1] & 0xF) << 2) | (raw[si + 2] >> 6)]), b64_buf[so + 2]);
                    try std.testing.expectEqual(@as(u16, alpha[raw[si + 2] & 0x3F]), b64_buf[so + 3]);
                }

                // Decode back and verify exact byte equivalence
                const dec_out = try alloc.alloc(u8, len);
                defer alloc.free(dec_out);
                var br: usize = 0;
                var bw: usize = 0;
                const mode: LastChunkHandling = if (omit_pad) .loose else .strict;
                const bst = zig_u8a_decode_base64(
                    if (enc_len == 0) null else b64_buf.ptr,
                    enc_len,
                    if (len == 0) null else dec_out.ptr,
                    len,
                    @intFromBool(is_url),
                    @intFromEnum(mode),
                    &br,
                    &bw,
                );
                try std.testing.expectEqual(@intFromEnum(Status.ok), bst);
                try std.testing.expectEqual(enc_len, br);
                try std.testing.expectEqual(len, bw);
                try std.testing.expectEqualSlices(u8, raw, dec_out);

                // Also verify null-dst validation/count mode matches
                var br2: usize = 0;
                var bw2: usize = 0;
                const bst2 = zig_u8a_decode_base64(
                    if (enc_len == 0) null else b64_buf.ptr,
                    enc_len,
                    null,
                    std.math.maxInt(usize),
                    @intFromBool(is_url),
                    @intFromEnum(mode),
                    &br2,
                    &bw2,
                );
                try std.testing.expectEqual(@intFromEnum(Status.ok), bst2);
                try std.testing.expectEqual(enc_len, br2);
                try std.testing.expectEqual(len, bw2);
            }
        }
    }
}
