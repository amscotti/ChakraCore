#!/usr/bin/env python3
# -------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
# -------------------------------------------------------------------------------------------------------
"""Patch a serialized-bytecode cache file's version stamps.

The serializer writes byteCodeCacheReleaseFileVersion as four DWORDs
(ByteCodeSerializer.cpp:526-535) = the GUID's in-memory layout, which is
exactly uuid.bytes_le. Replacing those 16 bytes with a different GUID's
bytes_le reproduces, byte-for-byte, what a build carrying that other GUID
would have written in the same fields. Used by the b5-guid-rotation
receipt to prove a stale-GUID cache is rejected, not misread.
"""
import sys
import uuid

def main():
    if len(sys.argv) != 4:
        print("usage: patch_guid.py <cache-file> <from-guid> <to-guid>")
        return 2
    path, frm, to = sys.argv[1:4]
    frm_b = uuid.UUID(frm).bytes_le
    to_b = uuid.UUID(to).bytes_le
    with open(path, "rb") as f:
        data = f.read()
    off = data.find(frm_b)
    if off < 0:
        print("patch_guid: version stamps of %s not found in %s" % (frm, path))
        return 1
    if data.find(frm_b, off + 1) >= 0:
        print("patch_guid: version stamps occur more than once; refusing")
        return 1
    with open(path, "wb") as f:
        f.write(data[:off] + to_b + data[off + 16:])
    print("patched GUID stamps at file offset %d (%s -> %s)" % (off, frm, to))
    return 0

if __name__ == "__main__":
    sys.exit(main())
