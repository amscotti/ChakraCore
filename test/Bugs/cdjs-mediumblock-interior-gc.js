//-------------------------------------------------------------------------------------------------------
// Copyright (c) 2026 ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Regression test for SmallHeapBlockT::GetRealAddressFromInterior on 16 KiB-page hosts
// (e.g. Apple Silicon macOS), where MediumAllocationBlockAttributes::PageCount * PageSize
// is 128 KiB (> 65535). Truncating (rawInteriorAddress - baseAddress) to ushort before
// >> ObjectAllocationShift wrapped upper-64 KiB objects back to lower-64 KiB indices, leaving
// upper-64 KiB SparseArraySegment<Var> / Medium objects unmarked on stack scan or skipping scan
// of their children.

(function () {
    "use strict";

    function makeMediumArrays(count, len, salt) {
        var holder = new Array(count);
        for (var i = 0; i < count; i++) {
            var seg = new Array(len);
            for (var j = 0; j < len; j++) {
                seg[j] = {
                    tag: salt,
                    bucket: i,
                    index: j,
                    nested: { check: ((i * 131 + j) ^ salt) }
                };
            }
            holder[i] = seg;
        }
        return holder;
    }

    function churnHeap(rounds) {
        for (var r = 0; r < rounds; r++) {
            // Allocate medium-block segments (2 KiB - 5 KiB) and small DynamicObjects
            // to force GC cycles naturally without requiring -CollectGarbage.
            var tmp = new Array(580);
            for (var j = 0; j < 580; j += 8) {
                tmp[j] = { r: r, j: j, pair: { x: r + j, y: r - j } };
            }
            if ((r & 63) === 63) {
                var leaf = new ArrayBuffer(4096);
                var view = new Uint32Array(leaf);
                view[0] = r;
                if (view[0] !== r) {
                    throw new Error("Unexpected TypedArray value");
                }
            }
        }
        if (typeof CollectGarbage === "function") {
            CollectGarbage();
        }
    }

    // Fill multiple 128 KiB MediumHeapBlocks across two Medium size buckets (~2 KiB and ~5 KiB)
    // so live SparseArraySegment<Var> backing stores span both [0, 64 KiB) and [64 KiB, 128 KiB).
    var groups = [
        makeMediumArrays(40, 240, 0x1055),
        makeMediumArrays(36, 580, 0x20aa)
    ];

    // Hold medium segments / elements across C++ frames (Array.prototype.sort and
    // Array.prototype.forEach) while churning allocations to trigger conservative stack/interior
    // scanning during GC.
    for (var g = 0; g < groups.length; g++) {
        var holder = groups[g];
        for (var idx = 0; idx < holder.length; idx += 3) {
            var arr = holder[idx];
            var calls = 0;
            arr.sort(function (a, b) {
                calls++;
                if ((calls & 255) === 1) {
                    churnHeap(120);
                }
                return a.index - b.index;
            });

            arr.forEach(function (item, k) {
                if ((k & 127) === 17) {
                    churnHeap(90);
                }
                if (item.bucket !== idx || item.index !== k) {
                    throw new Error("Corrupted element in forEach at " + idx + "[" + k + "]");
                }
            });
        }
    }

    // Drop alternating arrays and refill to exercise partial/swept MediumHeapBlock reuse
    // in upper 64 KiB slots.
    for (var g2 = 0; g2 < groups.length; g2++) {
        var list = groups[g2];
        for (var k2 = 1; k2 < list.length; k2 += 4) {
            list[k2] = null;
        }
    }
    churnHeap(600);
    for (var g3 = 0; g3 < groups.length; g3++) {
        var list3 = groups[g3];
        var len3 = (g3 === 0) ? 240 : 580;
        var salt3 = (g3 === 0) ? 0x1055 : 0x20aa;
        for (var k3 = 1; k3 < list3.length; k3 += 4) {
            var rebuilt = new Array(len3);
            for (var j3 = 0; j3 < len3; j3++) {
                rebuilt[j3] = {
                    tag: salt3,
                    bucket: k3,
                    index: j3,
                    nested: { check: ((k3 * 131 + j3) ^ salt3) }
                };
            }
            list3[k3] = rebuilt;
        }
    }
    churnHeap(600);

    // Verify all surviving MediumHeapBlock segments and their child DynamicObjects.
    var salts = [0x1055, 0x20aa];
    for (var vg = 0; vg < groups.length; vg++) {
        var vlist = groups[vg];
        var vsalt = salts[vg];
        for (var vi = 0; vi < vlist.length; vi++) {
            var vseg = vlist[vi];
            for (var vj = 0; vj < vseg.length; vj++) {
                var obj = vseg[vj];
                if (!obj ||
                    obj.tag !== vsalt ||
                    obj.bucket !== vi ||
                    obj.index !== vj ||
                    !obj.nested ||
                    obj.nested.check !== ((vi * 131 + vj) ^ vsalt)) {
                    throw new Error("GC corruption at group=" + vg + " bucket=" + vi + " index=" + vj);
                }
            }
        }
    }

    print("PASS");
})();
