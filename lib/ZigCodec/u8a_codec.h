//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Flat C ABI for the ES2026 Uint8Array Base64 / Hex Z1 Zig SIMD codec.
// Pure caller-owned buffers: zero heap allocation, zero Recycler pointers, zero exceptions.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zig_u8a_status_t {
    ZIG_U8A_OK = 0,
    ZIG_U8A_SYNTAX_ERROR = 1,
} zig_u8a_status_t;

typedef enum zig_u8a_last_chunk_t {
    ZIG_U8A_LAST_LOOSE = 0,
    ZIG_U8A_LAST_STRICT = 1,
    ZIG_U8A_LAST_STOP_BEFORE_PARTIAL = 2,
} zig_u8a_last_chunk_t;

// Encodes src[0..src_len] into lowercase ASCII hex UTF-16 code units at dst[0..src_len * 2].
void zig_u8a_encode_hex(const uint8_t* src, size_t src_len, uint16_t* dst);

// Decodes strict ES2026 hex UTF-16 code units src[0..src_len] into dst[0..dst_cap].
// If dst == NULL, validates and counts decoded bytes without writing memory.
// Odd src_len returns ZIG_U8A_SYNTAX_ERROR with *out_read = 0, *out_written = 0 without writing dst.
// Invalid characters in an even-length string leave valid prefix pairs written up to the error.
uint8_t zig_u8a_decode_hex(
    const uint16_t* src,
    size_t src_len,
    uint8_t* dst,
    size_t dst_cap,
    size_t* out_read,
    size_t* out_written);

// Returns exact UTF-16 code unit count for Base64 encoding, or SIZE_MAX on size_t overflow.
size_t zig_u8a_encode_base64_len(size_t src_len, uint8_t omit_padding);

// Encodes src[0..src_len] into Base64 (url_alphabet == 0) or Base64URL (url_alphabet != 0)
// UTF-16 code units at dst, omitting '=' padding when omit_padding != 0.
// Returns the number of UTF-16 code units written.
size_t zig_u8a_encode_base64(
    const uint8_t* src,
    size_t src_len,
    uint16_t* dst,
    uint8_t url_alphabet,
    uint8_t omit_padding);

// Returns an upper bound on decoded byte count for a Base64 input of src_len UTF-16 code units.
size_t zig_u8a_decode_base64_max_len(size_t src_len);

// Decodes ES2026 Base64 / Base64URL UTF-16 code units src[0..src_len] into dst[0..dst_cap].
// If dst == NULL, validates and counts decoded bytes without writing memory.
// Writes decoded bytes directly to dst as chunks complete, so SyntaxError leaves valid prefix bytes written.
uint8_t zig_u8a_decode_base64(
    const uint16_t* src,
    size_t src_len,
    uint8_t* dst,
    size_t dst_cap,
    uint8_t url_alphabet,
    uint8_t last_chunk_mode,
    size_t* out_read,
    size_t* out_written);

#ifdef __cplusplus
}
#endif
