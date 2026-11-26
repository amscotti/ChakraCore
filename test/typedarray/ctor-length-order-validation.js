//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assertEq(actual, expected, msg) {
    if (actual !== expected) {
        throw new Error(msg + ": expected " + String(expected) + ", got " + String(actual));
    }
}

function assertThrows(fn, ErrCtor, msg) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = (e instanceof ErrCtor);
        if (!threw) {
            throw new Error(msg + ": expected " + ErrCtor.name + ", got " + e);
        }
    }
    if (!threw) {
        throw new Error(msg + ": expected " + ErrCtor.name + ", did not throw");
    }
}

// RV-13: TypedArray(buffer, byteOffset, length) validation order.
// 1. Offset > buffer.byteLength must still coerce args[3] before throwing RangeError.
var calls1 = 0;
assertThrows(function () {
    new Uint8Array(new ArrayBuffer(4), 8, {
        valueOf: function () { calls1++; return 0; }
    });
}, RangeError, "offset > byteLength throws RangeError");
assertEq(calls1, 1, "args[3] coerced before offset > byteLength check");

// Custom error thrown during args[3] coercion wins over offset > byteLength.
assertThrows(function () {
    new Uint16Array(new ArrayBuffer(4), 8, {
        valueOf: function () { throw new URIError("len-coerce"); }
    });
}, URIError, "args[3] ToIndex error precedes offset > byteLength");

// 2. Unaligned byteOffset (offset % elementSize != 0) runs BEFORE args[3] ToIndex.
var calls2 = 0;
assertThrows(function () {
    new Uint16Array(new ArrayBuffer(4), 1, {
        valueOf: function () { calls2++; return 0; }
    });
}, RangeError, "unaligned byteOffset throws RangeError");
assertEq(calls2, 0, "unaligned byteOffset rejects before args[3] ToIndex");

// 3. Reentrancy during args[3] ToIndex: detaching or shrinking/growing the buffer.
var abDetach = new ArrayBuffer(4);
assertThrows(function () {
    new Uint16Array(abDetach, 2, {
        valueOf: function () {
            ArrayBuffer.detach(abDetach);
            return 0;
        }
    });
}, TypeError, "detaching buffer during length ToIndex throws TypeError");

var rabGrow = new ArrayBuffer(4, { maxByteLength: 16 });
var taGrown = new Uint8Array(rabGrow, 8, {
    valueOf: function () {
        rabGrow.resize(16);
        return 2;
    }
});
assertEq(taGrown.length, 2, "growing RAB during length ToIndex allows offset > initial byteLength");
assertEq(taGrown.byteOffset, 8, "grown view keeps byteOffset");

var rabShrink = new ArrayBuffer(8, { maxByteLength: 16 });
assertThrows(function () {
    new Uint16Array(rabShrink, 2, {
        valueOf: function () {
            rabShrink.resize(0);
            return 0;
        }
    });
}, RangeError, "shrinking RAB past offset during length ToIndex throws RangeError");

// Trailing-byte alignment check only applies when length is omitted/undefined.
assertThrows(function () {
    new Uint16Array(new ArrayBuffer(5), 0, undefined);
}, RangeError, "unaligned buffer with undefined length throws RangeError");
var taExplicit = new Uint16Array(new ArrayBuffer(5), 0, 2);
assertEq(taExplicit.length, 2, "explicit length on unaligned buffer succeeds");

// PE-1: GetLength() fast-path preserves RAB shrink/grow, GSAB grow, and detach.
var fixed = new Int32Array([10, 20, 30, 40]);
assertEq(fixed.length, 4, "fixed view initial length");
ArrayBuffer.detach(fixed.buffer);
assertEq(fixed.length, 0, "detached fixed view length becomes 0");
assertEq(fixed[0], undefined, "detached element read is undefined");

var rab = new ArrayBuffer(8, { maxByteLength: 16 });
var trackView = new Int32Array(rab, 4);
var fixedView = new Int32Array(rab, 4, 1);
assertEq(trackView.length, 1, "tracking view initial length");
assertEq(fixedView.length, 1, "fixed view initial length");

rab.resize(16);
assertEq(trackView.length, 3, "tracking view grows on resize");
assertEq(fixedView.length, 1, "fixed view keeps captured length on grow");

rab.resize(4);
assertEq(trackView.length, 0, "tracking view shrinks to 0 at byteOffset boundary");
assertEq(fixedView.length, 0, "fixed view OOB reports length 0");

rab.resize(2);
assertEq(trackView.length, 0, "tracking view OOB below byteOffset reports 0");
assertEq(fixedView.length, 0, "fixed view OOB below byteOffset reports 0");

rab.resize(12);
assertEq(trackView.length, 2, "tracking view recovers on re-grow");
assertEq(fixedView.length, 1, "fixed view recovers on re-grow");

var gsab = new SharedArrayBuffer(8, { maxByteLength: 16 });
var gsabTrack = new Int32Array(gsab);
var gsabFixed = new Int32Array(gsab, 0, 2);
assertEq(gsabTrack.length, 2, "GSAB tracking initial length");
assertEq(gsabFixed.length, 2, "GSAB fixed initial length");
gsab.grow(16);
assertEq(gsabTrack.length, 4, "GSAB tracking grows live");
assertEq(gsabFixed.length, 2, "GSAB fixed keeps captured length");

print("pass");
