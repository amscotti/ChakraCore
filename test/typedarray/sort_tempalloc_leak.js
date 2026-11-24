//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("../UnitTestFramework/UnitTestFramework.js");

function assertPrefix(actual, expectedPrefix, msg) {
    assert.isTrue(actual.length >= expectedPrefix.length, msg + " length");
    for (let i = 0; i < expectedPrefix.length; i++) {
        assert.areEqual(expectedPrefix[i], actual[i], msg + " [" + i + "]");
    }
}

const tests = [
    {
        name: "Repeated large TypedArray.prototype.sort calls complete and reuse temp arena",
        body() {
            // Keep 1 MiB iterations small so Debug/ASan stay well under runner timeouts,
            // while exercising > MaxTemporaryArenaAllocators (5) on 64 KiB MergeSort.
            for (let iter = 0; iter < 3; iter++) {
                const ta = new Uint8Array(1 << 20);
                ta[0] = 9;
                ta[1] = 3;
                ta[ta.length - 1] = 1;
                const ret = ta.sort((a, b) => a - b);
                assert.isTrue(ret === ta, "sort returns receiver");
                assert.areEqual(1 << 20, ta.length, "length preserved");
                assert.areEqual(0, ta[0], "zeros sort before positive values");
                assert.areEqual(1, ta[ta.length - 3], "sorted 1");
                assert.areEqual(3, ta[ta.length - 2], "sorted 3");
                assert.areEqual(9, ta[ta.length - 1], "sorted 9");
            }
            for (let iter = 0; iter < 12; iter++) {
                const ta64k = new Uint8Array(1 << 16);
                ta64k[0] = 7;
                ta64k[ta64k.length - 1] = 2;
                ta64k.sort((a, b) => a - b);
                assert.areEqual(0, ta64k[0], "64k zero");
                assert.areEqual(2, ta64k[ta64k.length - 2], "64k 2");
                assert.areEqual(7, ta64k[ta64k.length - 1], "64k 7");
            }

            // Default comparator path (compareFn === undefined) on MergeSort (> 512)
            // and InsertionSort (< 512) lengths across pool wrap-around.
            for (let iter = 0; iter < 16; iter++) {
                const med = new Int32Array(1024);
                for (let i = 0; i < med.length; i++) {
                    med[i] = med.length - i;
                }
                med.sort();
                assert.areEqual(1, med[0], "default mergesort min");
                assert.areEqual(1024, med[1023], "default mergesort max");

                const small = new Uint8Array([5, 1, 4, 2, 3]);
                small.sort();
                assert.areEqual(Uint8Array.from([1, 2, 3, 4, 5]), small, "default insertionsort");
            }

            // Nested reentrant sort (> MaxTemporaryArenaAllocators = 5) exercises pool overflow disposal.
            function nestedSort(depth) {
                const arr = new Uint16Array(600);
                arr[0] = 10;
                arr[599] = 2;
                let descended = false;
                arr.sort((a, b) => {
                    if (!descended && depth > 0) {
                        descended = true;
                        nestedSort(depth - 1);
                    }
                    return a - b;
                });
                assert.areEqual(0, arr[0], "nested sort zero");
                assert.areEqual(2, arr[598], "nested sort 2");
                assert.areEqual(10, arr[599], "nested sort 10");
            }
            nestedSort(7);
        }
    },
    {
        name: "Throwing comparator releases temporary arena and subsequent sorts remain healthy",
        body() {
            // MergeSort (>= 512) throws partway through; buffer must stay untouched and arena released.
            for (let iter = 0; iter < 16; iter++) {
                const ta = new Uint8Array(1024);
                for (let i = 0; i < ta.length; i++) {
                    ta[i] = (1024 - i) & 0xff;
                }
                const firstBefore = ta[0];
                const lastBefore = ta[1023];
                let calls = 0;
                assert.throws(() => {
                    ta.sort((a, b) => {
                        calls++;
                        if (calls === 50) {
                            throw new Error("comparator boom");
                        }
                        return a - b;
                    });
                }, Error, "throwing comparator propagates");
                assert.areEqual(firstBefore, ta[0], "receiver untouched at 0 after throw");
                assert.areEqual(lastBefore, ta[1023], "receiver untouched at 1023 after throw");
            }

            // InsertionSort (< 512) throw path.
            const small = new Int16Array([4, 2, 3, 1]);
            assert.throws(() => {
                small.sort(() => { throw new RangeError("small boom"); });
            }, RangeError, "small sort throw propagates");
            assert.areEqual(Int16Array.from([4, 2, 3, 1]), small, "small receiver untouched after throw");

            // Subsequent repeated normal sorts succeed cleanly.
            for (let iter = 0; iter < 16; iter++) {
                const ok = new Uint16Array(1024);
                for (let i = 0; i < ok.length; i++) {
                    ok[i] = 1023 - i;
                }
                ok.sort((a, b) => a - b);
                assert.areEqual(0, ok[0], "post-throw sort first");
                assert.areEqual(1023, ok[1023], "post-throw sort last");
            }
        }
    },
    {
        name: "Comparator detaching or shrinking a resizable ArrayBuffer during sort",
        body() {
            // Detach during small and MergeSort comparator calls throws TypeError and releases arena.
            const taSmall = new Uint8Array([3, 1, 2, 0]);
            let detachedSmall = false;
            assert.throws(() => {
                taSmall.sort((a, b) => {
                    if (!detachedSmall) {
                        detachedSmall = true;
                        ArrayBuffer.detach(taSmall.buffer);
                    }
                    return a - b;
                });
            }, TypeError, "detach during small sort throws TypeError");
            assert.areEqual(0, taSmall.length, "detached small view has length 0");

            for (let iter = 0; iter < 8; iter++) {
                const taLarge = new Uint8Array(1024);
                taLarge[0] = 9;
                taLarge[1] = 2;
                let detachedLarge = false;
                assert.throws(() => {
                    taLarge.sort((a, b) => {
                        if (!detachedLarge) {
                            detachedLarge = true;
                            ArrayBuffer.detach(taLarge.buffer);
                        }
                        return a - b;
                    });
                }, TypeError, "detach during mergesort throws TypeError");
                assert.areEqual(0, taLarge.length, "detached large view has length 0");
            }

            // Length-tracking view shrunk mid-sort writes only surviving indices.
            const rab = new ArrayBuffer(8, { maxByteLength: 16 });
            const ltView = new Uint8Array(rab);
            ltView.set([8, 7, 6, 5, 4, 3, 2, 1]);
            let shrunk = false;
            const ltRet = ltView.sort((a, b) => {
                if (!shrunk) {
                    shrunk = true;
                    rab.resize(4);
                }
                return a - b;
            });
            assert.isTrue(ltRet === ltView, "shrunk length-tracking sort returns receiver");
            assert.areEqual(4, ltView.length, "length-tracking view reflects shrunk size");
            assertPrefix(ltView, [1, 2, 3, 4], "shrunk length-tracking view receives sorted prefix");

            // Length-tracking view shrunk to zero mid-sort.
            const rabZero = new ArrayBuffer(8, { maxByteLength: 16 });
            const ltZero = new Uint8Array(rabZero);
            ltZero.set([5, 4, 3, 2, 1, 0, 9, 8]);
            let zeroed = false;
            ltZero.sort((a, b) => {
                if (!zeroed) {
                    zeroed = true;
                    rabZero.resize(0);
                }
                return a - b;
            });
            assert.areEqual(0, ltZero.length, "shrunk-to-zero view has length 0");

            // Large length-tracking view (> 512) shrunk mid-sort.
            const rabLarge = new ArrayBuffer(1024, { maxByteLength: 2048 });
            const ltLarge = new Uint8Array(rabLarge);
            for (let i = 0; i < 1024; i++) {
                ltLarge[i] = (1024 - i) & 0xff;
            }
            let shrunkLarge = false;
            ltLarge.sort((a, b) => {
                if (!shrunkLarge) {
                    shrunkLarge = true;
                    rabLarge.resize(256);
                }
                return a - b;
            });
            assert.areEqual(256, ltLarge.length, "shrunk large view has length 256");
            assert.areEqual(0, ltLarge[0], "shrunk large view sorted min");
            assert.areEqual(0, ltLarge[3], "shrunk large view has 4 zeros at front");
            assert.areEqual(1, ltLarge[4], "shrunk large view monotonic");

            // Fixed-length view pushed out-of-bounds mid-sort leaves retained bytes untouched.
            const rabFixed = new ArrayBuffer(8, { maxByteLength: 16 });
            const fixedView = new Uint8Array(rabFixed, 2, 4);
            fixedView.set([40, 10, 30, 20]);
            let oob = false;
            fixedView.sort((a, b) => {
                if (!oob) {
                    oob = true;
                    rabFixed.resize(4);
                }
                return a - b;
            });
            assert.areEqual(0, fixedView.length, "OOB fixed view reports length 0");
            rabFixed.resize(8);
            assert.areEqual(4, fixedView.length, "regrown fixed view recovers length");
            // Surviving bytes [2..3] kept unsorted [40, 10] (not sorted [10, 20]);
            // bytes [4..5] were zeroed by rabFixed.resize(8) per spec.
            assertPrefix(fixedView, [40, 10, 0, 0], "OOB fixed view retained bytes unchanged");
        }
    },
    {
        name: "TypedArray.prototype.with preserves ES2025 coercion before live index validation",
        body() {
            // Length-tracking view grown during valueOf makes previously OOB index 10 valid.
            const ab = new ArrayBuffer(4, { maxByteLength: 16 });
            const ta = new Uint8Array(ab);
            ta.set([1, 2, 3, 4]);
            let called = 0;
            const res = ta.with(10, {
                valueOf() {
                    called++;
                    ab.resize(16);
                    return 7;
                }
            });
            assert.areEqual(1, called, "RAB with calls valueOf once");
            assert.isTrue(res instanceof Uint8Array, "RAB with returns Uint8Array");
            assert.areEqual(4, res.length, "RAB with returns captured length 4");
            assertPrefix(res, [1, 2, 3, 4], "RAB with copies captured length elements");

            // Fixed-length view runs ToNumber before throwing RangeError.
            const fixed = new Uint8Array([1, 2, 3, 4]);
            let fixedCalled = 0;
            assert.throws(() => {
                fixed.with(10, {
                    valueOf() {
                        fixedCalled++;
                        return 7;
                    }
                });
            }, RangeError, "fixed-length with(10, ...) throws RangeError");
            assert.areEqual(1, fixedCalled, "fixed-length with still executes valueOf once");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
