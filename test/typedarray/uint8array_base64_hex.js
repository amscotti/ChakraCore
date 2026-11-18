//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function toArray(u8) {
    const out = [];
    for (let i = 0; i < u8.length; i++) {
        out.push(u8[i]);
    }
    return out;
}

// Minimal scalar reference encoder/decoder for SIMD boundary cross-checking (0..512 bytes).
function refEncodeHex(bytes) {
    const hex = "0123456789abcdef";
    let s = "";
    for (let i = 0; i < bytes.length; i++) {
        s += hex[bytes[i] >> 4] + hex[bytes[i] & 0x0f];
    }
    return s;
}

function refEncodeBase64(bytes, urlAlphabet, omitPadding) {
    const alpha = urlAlphabet
        ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
        : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let out = "";
    let i = 0;
    for (; i + 3 <= bytes.length; i += 3) {
        const b0 = bytes[i];
        const b1 = bytes[i + 1];
        const b2 = bytes[i + 2];
        out += alpha[b0 >> 2]
            + alpha[((b0 & 0x03) << 4) | (b1 >> 4)]
            + alpha[((b1 & 0x0f) << 2) | (b2 >> 6)]
            + alpha[b2 & 0x3f];
    }
    const rem = bytes.length - i;
    if (rem === 1) {
        const b0 = bytes[i];
        out += alpha[b0 >> 2] + alpha[(b0 & 0x03) << 4];
        if (!omitPadding) out += "==";
    } else if (rem === 2) {
        const b0 = bytes[i];
        const b1 = bytes[i + 1];
        out += alpha[b0 >> 2] + alpha[((b0 & 0x03) << 4) | (b1 >> 4)] + alpha[(b1 & 0x0f) << 2];
        if (!omitPadding) out += "=";
    }
    return out;
}

const tests = [
    {
        name: "Property descriptors and non-constructor behavior",
        body: function () {
            for (const [obj, name, len] of [
                [Uint8Array, "fromBase64", 1],
                [Uint8Array, "fromHex", 1],
                [Uint8Array.prototype, "toBase64", 0],
                [Uint8Array.prototype, "setFromBase64", 1],
                [Uint8Array.prototype, "toHex", 0],
                [Uint8Array.prototype, "setFromHex", 1]
            ]) {
                const desc = Object.getOwnPropertyDescriptor(obj, name);
                assert.isTrue(typeof desc.value === "function", name + " is function");
                assert.areEqual(false, desc.enumerable, name + " non-enumerable");
                assert.areEqual(true, desc.writable, name + " writable");
                assert.areEqual(true, desc.configurable, name + " configurable");
                assert.areEqual(name, desc.value.name, name + " .name");
                assert.areEqual(len, desc.value.length, name + " .length");
                assert.throws(() => new desc.value(), TypeError, name + " is not constructor");
            }
        }
    },
    {
        name: "RFC 4648 vectors and base64url / omitPadding / hex roundtrips",
        body: function () {
            const rfc = [
                ["", "", "", "", "", ""],
                ["f", "Zg==", "Zg", "Zg==", "Zg", "66"],
                ["fo", "Zm8=", "Zm8", "Zm8=", "Zm8", "666f"],
                ["foo", "Zm9v", "Zm9v", "Zm9v", "Zm9v", "666f6f"],
                ["foob", "Zm9vYg==", "Zm9vYg", "Zm9vYg==", "Zm9vYg", "666f6f62"],
                ["fooba", "Zm9vYmE=", "Zm9vYmE", "Zm9vYmE=", "Zm9vYmE", "666f6f6261"],
                ["foobar", "Zm9vYmFy", "Zm9vYmFy", "Zm9vYmFy", "Zm9vYmFy", "666f6f626172"]
            ];
            for (const [plain, b64, b64NoPad, b64u, b64uNoPad, hex] of rfc) {
                const bytes = new Uint8Array(plain.length);
                for (let i = 0; i < plain.length; i++) bytes[i] = plain.charCodeAt(i);
                assert.areEqual(b64, bytes.toBase64());
                assert.areEqual(b64NoPad, bytes.toBase64({ omitPadding: true }));
                assert.areEqual(b64u, bytes.toBase64({ alphabet: "base64url" }));
                assert.areEqual(b64uNoPad, bytes.toBase64({ alphabet: "base64url", omitPadding: true }));
                assert.areEqual(hex, bytes.toHex());
                assert.areEqual(toArray(bytes), toArray(Uint8Array.fromBase64(b64)));
                assert.areEqual(toArray(bytes), toArray(Uint8Array.fromBase64(b64NoPad)));
                assert.areEqual(toArray(bytes), toArray(Uint8Array.fromBase64(b64u, { alphabet: "base64url" })));
                assert.areEqual(toArray(bytes), toArray(Uint8Array.fromHex(hex)));
                assert.areEqual(toArray(bytes), toArray(Uint8Array.fromHex(hex.toUpperCase())));
            }

            const high = new Uint8Array([0xfb, 0xff, 0xff]);
            assert.areEqual("+///", high.toBase64());
            assert.areEqual("-___", high.toBase64({ alphabet: "base64url" }));
            assert.areEqual([0xfb, 0xff, 0xff], toArray(Uint8Array.fromBase64("-___", { alphabet: "base64url" })));
            const alt = new Uint8Array([0xfb, 0xff, 0xbf]);
            assert.areEqual("+/+/", alt.toBase64());
            assert.areEqual("-_-_", alt.toBase64({ alphabet: "base64url" }));
            assert.areEqual([0xfb, 0xff, 0xbf], toArray(Uint8Array.fromBase64("-_-_", { alphabet: "base64url" })));
        }
    },
    {
        name: "SIMD boundaries 0..512 bytes for base64, base64url, and hex",
        body: function () {
            for (let len = 0; len <= 512; len++) {
                // Skip intermediate large steps? 0..512 is 513 iterations, <20ms total.
                const src = new Uint8Array(len);
                for (let i = 0; i < len; i++) {
                    src[i] = (i * 131 + 17) & 0xff;
                }
                const expectedHex = refEncodeHex(src);
                const actualHex = src.toHex();
                assert.areEqual(expectedHex, actualHex, "toHex len=" + len);
                const roundHex = Uint8Array.fromHex(actualHex.toUpperCase());
                assert.areEqual(toArray(src), toArray(roundHex), "fromHex len=" + len);

                for (const url of [false, true]) {
                    for (const omit of [false, true]) {
                        const opts = { alphabet: url ? "base64url" : "base64", omitPadding: omit };
                        const expectedB64 = refEncodeBase64(src, url, omit);
                        const actualB64 = src.toBase64(opts);
                        assert.areEqual(expectedB64, actualB64, "toBase64 len=" + len);
                        const roundB64 = Uint8Array.fromBase64(actualB64, { alphabet: opts.alphabet });
                        assert.areEqual(toArray(src), toArray(roundB64), "fromBase64 len=" + len);
                    }
                }
            }
        }
    },
    {
        name: "64KiB Uint8VirtualArray large-buffer and GC survival",
        body: function () {
            function maybeGC() {
                if (typeof CollectGarbage === "function") {
                    CollectGarbage();
                } else {
                    for (let g = 0; g < 64; g++) {
                        new Uint8Array(4096);
                    }
                }
            }
            const size = 65536 + 37;
            const src = new Uint8Array(size);
            for (let i = 0; i < size; i++) src[i] = (i * 29 + 3) & 0xff;
            const pow2 = new Uint8Array(65536);
            for (let i = 0; i < pow2.length; i++) pow2[i] = i & 0xff;

            // Compound concatenated string forces lazy Flatten on entry.
            let b64 = pow2.toBase64();
            b64 = b64.slice(0, b64.length / 2) + b64.slice(b64.length / 2);
            maybeGC();
            const decodedPow2 = Uint8Array.fromBase64(b64);
            maybeGC();
            assert.areEqual(65536, decodedPow2.length);
            for (let i = 0; i < 65536; i += 257) {
                assert.areEqual(pow2[i], decodedPow2[i]);
            }

            let hex = src.toHex();
            hex = hex.slice(0, 1000) + hex.slice(1000);
            maybeGC();
            const decodedSrc = Uint8Array.fromHex(hex);
            maybeGC();
            assert.areEqual(size, decodedSrc.length);
            for (let i = 0; i < size; i += 257) {
                assert.areEqual(src[i], decodedSrc[i]);
            }
            assert.areEqual(src[size - 1], decodedSrc[size - 1]);
        }
    },
    {
        name: "lastChunkHandling, whitespace, and non-ASCII rejection",
        body: function () {
            // Loose vs strict extra bits ("Zg==" clean, "Zh==" non-zero extra bits)
            assert.areEqual([102], toArray(Uint8Array.fromBase64("Zh==", { lastChunkHandling: "loose" })));
            assert.throws(() => Uint8Array.fromBase64("Zh==", { lastChunkHandling: "strict" }), SyntaxError);
            assert.throws(() => Uint8Array.fromBase64("Zg", { lastChunkHandling: "strict" }), SyntaxError);

            // stop-before-partial stops before partial trailing chunk
            const dst = new Uint8Array(8);
            dst.fill(0xff);
            const r = dst.setFromBase64("Zm9vZh", { lastChunkHandling: "stop-before-partial" });
            assert.areEqual(4, r.read);
            assert.areEqual(3, r.written);
            assert.areEqual([102, 111, 111, 0xff, 0xff, 0xff, 0xff, 0xff], toArray(dst));

            // Padded complete tail IS consumed under stop-before-partial
            const dst2 = new Uint8Array(8);
            const r2 = dst2.setFromBase64("Zm9vZg==", { lastChunkHandling: "stop-before-partial" });
            assert.areEqual(8, r2.read);
            assert.areEqual(4, r2.written);

            // Whitespace allowed: \t \n \f \r ' '
            const ws = "\t Z\nm\f9\rv ";
            assert.areEqual([102, 111, 111], toArray(Uint8Array.fromBase64(ws)));

            // Disallowed whitespace and surrogates
            for (const bad of ["\vZm9v", "\u00a0Zm9v", "Zm\uD800v", "Zm\u0100v"]) {
                assert.throws(() => Uint8Array.fromBase64(bad), SyntaxError);
            }
            // Hex forbids ALL whitespace
            for (const badHex of [" 66", "66\n", "6 6", "6", "6\u00a0"]) {
                assert.throws(() => Uint8Array.fromHex(badHex), SyntaxError);
            }
        }
    },
    {
        name: "writes-up-to-error and setFromHex odd-length invariance",
        body: function () {
            const b64Dst = new Uint8Array([0xee, 0xee, 0xee, 0xee, 0xee]);
            assert.throws(() => b64Dst.setFromBase64("Zm9vZm!v"), SyntaxError);
            // Valid first 4 chars decoded [102, 111, 111], second chunk aborted cleanly
            assert.areEqual([102, 111, 111, 0xee, 0xee], toArray(b64Dst));

            const hexDst = new Uint8Array([0xee, 0xee, 0xee, 0xee]);
            assert.throws(() => hexDst.setFromHex("aabbzz"), SyntaxError);
            assert.areEqual([0xaa, 0xbb, 0xee, 0xee], toArray(hexDst));

            // Odd length writes NOTHING even if valid prefix exists, and still throws on empty target
            const hexOdd = new Uint8Array([0xee, 0xee]);
            assert.throws(() => hexOdd.setFromHex("aab"), SyntaxError);
            assert.areEqual([0xee, 0xee], toArray(hexOdd));
            assert.throws(() => (new Uint8Array(0)).setFromHex("1"), SyntaxError);
        }
    },
    {
        name: "Options coercion order, non-coerced strings, and receiver guards",
        body: function () {
            let calls = [];
            const throwToString = { toString() { calls.push("toString"); return "base64"; } };
            assert.throws(() => Uint8Array.fromBase64(throwToString), TypeError);
            assert.throws(() => Uint8Array.fromHex(throwToString), TypeError);
            assert.throws(() => (new Uint8Array(4)).setFromBase64(throwToString), TypeError);
            assert.throws(() => (new Uint8Array(4)).setFromHex(throwToString), TypeError);
            assert.areEqual(0, calls.length);

            for (const badOpts of [null, true, false, 123, "str", Symbol("s"), 1n]) {
                assert.throws(() => (new Uint8Array(1)).toBase64(badOpts), TypeError);
                assert.throws(() => (new Uint8Array(1)).setFromBase64("Zg==", badOpts), TypeError);
                assert.throws(() => Uint8Array.fromBase64("Zg==", badOpts), TypeError);
            }

            // alphabet getter must run before omitPadding getter
            const order = [];
            const opts = {
                get alphabet() { order.push("alphabet"); return "base64"; },
                get omitPadding() { order.push("omitPadding"); return true; },
                get lastChunkHandling() { order.push("lastChunkHandling"); return "loose"; }
            };
            assert.areEqual("Zg", (new Uint8Array([102])).toBase64(opts));
            assert.areEqual(["alphabet", "omitPadding"], order);
            order.length = 0;
            (new Uint8Array(4)).setFromBase64("Zg==", opts);
            assert.areEqual(["alphabet", "lastChunkHandling"], order);

            // Non-Uint8Array receivers rejected (Uint8ClampedArray, Int8Array, Array)
            const clamped = new Uint8ClampedArray([1, 2]);
            assert.throws(() => Uint8Array.prototype.toBase64.call(clamped), TypeError);
            assert.throws(() => Uint8Array.prototype.toHex.call(clamped), TypeError);
            assert.throws(() => Uint8Array.prototype.setFromBase64.call(clamped, "AQ=="), TypeError);
            assert.throws(() => Uint8Array.prototype.setFromHex.call(clamped, "01"), TypeError);
        }
    },
    {
        name: "Immutable ArrayBuffer, transfer() detach in getter, and RAB resize in getter",
        body: function () {
            if (typeof ArrayBuffer.prototype.transferToImmutable === "function") {
                const imm = (new Uint8Array([102, 111, 111])).buffer.transferToImmutable();
                const immView = new Uint8Array(imm);
                assert.areEqual("Zm9v", immView.toBase64());
                assert.areEqual("666f6f", immView.toHex());

                const immCalls = [];
                const badStr = { toString() { immCalls.push("str"); return "AQ=="; } };
                const spyOpts = { get alphabet() { immCalls.push("alpha"); return "base64"; } };
                assert.throws(() => immView.setFromBase64(badStr, spyOpts), TypeError);
                assert.throws(() => immView.setFromHex(badStr), TypeError);
                assert.areEqual(0, immCalls.length, "immutable check before reading args");
                assert.areEqual([102, 111, 111], toArray(immView));
            }

            if (typeof ArrayBuffer.prototype.transfer === "function") {
                const ab1 = new ArrayBuffer(4);
                const u8_1 = new Uint8Array(ab1);
                u8_1.set([1, 2, 3, 4]);
                let detachedCalls = 0;
                assert.throws(() => u8_1.toBase64({
                    get alphabet() {
                        detachedCalls++;
                        ab1.transfer();
                        return "base64";
                    }
                }), TypeError);
                assert.areEqual(1, detachedCalls);

                const ab2 = new ArrayBuffer(4);
                const u8_2 = new Uint8Array(ab2);
                let setDetachCalls = 0;
                assert.throws(() => u8_2.setFromBase64("AQIDBA==", {
                    get lastChunkHandling() {
                        setDetachCalls++;
                        ab2.transfer();
                        return "loose";
                    }
                }), TypeError);
                assert.areEqual(1, setDetachCalls);
            }

            // ResizableArrayBuffer grow & shrink/OOB inside options getter
            const rab = new ArrayBuffer(4, { maxByteLength: 32 });
            const rabView = new Uint8Array(rab);
            rabView.set([102, 111, 111, 98]);
            const encodedGrown = rabView.toBase64({
                get alphabet() {
                    rab.resize(6);
                    rabView[4] = 97;
                    rabView[5] = 114;
                    return "base64";
                }
            });
            assert.areEqual("Zm9vYmFy", encodedGrown);

            // Shrink to zero via length-tracking view
            const shrinkRes = rabView.toBase64({
                get omitPadding() {
                    rab.resize(0);
                    return false;
                }
            });
            assert.areEqual("", shrinkRes);

            // Grow again and write into grown bytes during setFromBase64 getter
            const setRes = rabView.setFromBase64("Zm9vYmFy", {
                get alphabet() {
                    rab.resize(8);
                    return "base64";
                }
            });
            assert.areEqual(8, setRes.read);
            assert.areEqual(6, setRes.written);
            assert.areEqual([102, 111, 111, 98, 97, 114, 0, 0], toArray(rabView));

            // Fixed-length view on RAB pushed out-of-bounds during options getter
            const fixedView = new Uint8Array(rab, 2, 4);
            assert.throws(() => fixedView.toBase64({
                get alphabet() {
                    rab.resize(3);
                    return "base64";
                }
            }), TypeError);
            assert.throws(() => fixedView.setFromBase64("Zg==", {
                get alphabet() {
                    return "base64";
                }
            }), TypeError);
        }
    },
    {
        name: "Cross-realm Uint8Array instances",
        body: function () {
            const other = WScript.LoadScript(
                "var foreign = new Uint8Array([102, 111, 111]); " +
                "function getForeign() { return foreign; } " +
                "var FUint8 = Uint8Array;",
                "samethread"
            );
            const foreign = other.getForeign();
            assert.areEqual("Zm9v", Uint8Array.prototype.toBase64.call(foreign));
            assert.areEqual("666f6f", Uint8Array.prototype.toHex.call(foreign));

            const w1 = Uint8Array.prototype.setFromBase64.call(foreign, "YmFy");
            assert.areEqual(4, w1.read);
            assert.areEqual(3, w1.written);
            assert.areEqual([98, 97, 114], [foreign[0], foreign[1], foreign[2]]);

            const w2 = Uint8Array.prototype.setFromHex.call(foreign, "aabbcc");
            assert.areEqual(6, w2.read);
            assert.areEqual(3, w2.written);
            assert.areEqual([0xaa, 0xbb, 0xcc], [foreign[0], foreign[1], foreign[2]]);

            const created = other.FUint8.fromBase64("Zg==");
            assert.areEqual(1, created.length);
            assert.areEqual(102, created[0]);
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
