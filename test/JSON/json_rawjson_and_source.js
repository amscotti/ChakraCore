//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assertSame(actual, expected, msg) {
    if (!Object.is(actual, expected)) {
        throw new Error((msg || "assertSame") + ": expected " + String(expected) + ", got " + String(actual));
    }
}

function assertTrue(cond, msg) {
    if (!cond) {
        throw new Error(msg || "assertion failed");
    }
}

function assertArrayEqual(actual, expected, msg) {
    assertSame(actual.length, expected.length, (msg || "array") + " length");
    for (let i = 0; i < expected.length; i++) {
        assertSame(actual[i], expected[i], (msg || "array") + "[" + i + "]");
    }
}

function assertThrows(ctor, fn, msg) {
    let threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        if (!(e instanceof ctor)) {
            throw new Error((msg || "assertThrows") + ": expected " + ctor.name + ", got " + e);
        }
    }
    if (!threw) {
        throw new Error((msg || "assertThrows") + ": expected " + ctor.name + " to be thrown");
    }
}

function gcNow() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

// 1. Built-in descriptors, lengths, names, and non-constructibility
(function testBuiltInMetadata() {
    for (const [prop, len] of [["rawJSON", 1], ["isRawJSON", 1]]) {
        const fn = JSON[prop];
        assertSame(typeof fn, "function", prop + " typeof");
        const fnDesc = Object.getOwnPropertyDescriptor(JSON, prop);
        assertTrue(fnDesc.writable && !fnDesc.enumerable && fnDesc.configurable, prop + " descriptor");

        const lenDesc = Object.getOwnPropertyDescriptor(fn, "length");
        assertSame(lenDesc.value, len, prop + ".length");
        assertTrue(!lenDesc.writable && !lenDesc.enumerable && lenDesc.configurable, prop + ".length attrs");

        const nameDesc = Object.getOwnPropertyDescriptor(fn, "name");
        assertSame(nameDesc.value, prop, prop + ".name");
        assertTrue(!nameDesc.writable && !nameDesc.enumerable && nameDesc.configurable, prop + ".name attrs");

        assertSame(fn.prototype, undefined, prop + ".prototype");
        assertThrows(TypeError, () => new fn("1"), "new JSON." + prop);
    }
})();

// 2. JSON.rawJSON object shape and valid inputs
(function testRawJSONValid() {
    function checkRaw(raw, expectedStr) {
        assertTrue(JSON.isRawJSON(raw), "isRawJSON");
        assertSame(Object.getPrototypeOf(raw), null, "null prototype");
        assertTrue(Object.isFrozen(raw), "frozen");
        assertArrayEqual(Object.getOwnPropertyNames(raw), ["rawJSON"], "own names");
        assertArrayEqual(Object.getOwnPropertySymbols(raw), [], "own symbols");
        assertArrayEqual(Reflect.ownKeys(raw), ["rawJSON"], "own keys");
        const desc = Object.getOwnPropertyDescriptor(raw, "rawJSON");
        assertSame(desc.value, expectedStr, "rawJSON value");
        assertTrue(!desc.writable && desc.enumerable && !desc.configurable, "rawJSON prop desc");
    }

    checkRaw(JSON.rawJSON(1), "1");
    checkRaw(JSON.rawJSON(-0), "0");
    checkRaw(JSON.rawJSON("-0"), "-0");
    checkRaw(JSON.rawJSON(-12.34e+5), "-1234000");
    checkRaw(JSON.rawJSON("-12.34e+5"), "-12.34e+5");
    checkRaw(JSON.rawJSON("0.0e0"), "0.0e0");
    checkRaw(JSON.rawJSON(null), "null");
    checkRaw(JSON.rawJSON(true), "true");
    checkRaw(JSON.rawJSON(false), "false");
    checkRaw(JSON.rawJSON('"foo\\n\\u0041"'), '"foo\\n\\u0041"');
    checkRaw(JSON.rawJSON('"\\u0000"'), '"\\u0000"');

    // Huge integers, BigInt, and double overflow numbers must succeed
    checkRaw(JSON.rawJSON("123456789012345678901234567890"), "123456789012345678901234567890");
    const big = BigInt(Number.MAX_SAFE_INTEGER) + 2n;
    checkRaw(JSON.rawJSON(big), "9007199254740993");
    checkRaw(JSON.rawJSON("1e1000"), "1e1000");
    checkRaw(JSON.rawJSON("-1e1000"), "-1e1000");
    const digits400 = "9".repeat(400);
    checkRaw(JSON.rawJSON(digits400), digits400);

    // Coercion via ToString, substrings, and concat strings
    checkRaw(JSON.rawJSON({ toString() { return "42"; } }), "42");
    checkRaw(JSON.rawJSON("  12345  ".slice(2, 7)), "12345");
    checkRaw(JSON.rawJSON("12" + "34"), "1234");
})();

// 3. JSON.rawJSON invalid inputs
(function testRawJSONInvalid() {
    assertThrows(TypeError, () => JSON.rawJSON(Symbol("1")), "Symbol throws TypeError");
    assertThrows(TypeError, () => JSON.rawJSON({ toString() { return Symbol("1"); } }), "toString Symbol throws TypeError");

    const invalidInputs = [
        undefined,
        "",
        " ",
        "\t1",
        "\n1",
        "\r1",
        " 1",
        "1\t",
        "1\n",
        "1\r",
        "1 ",
        "{}",
        "[]",
        '{"a":1}',
        "[1]",
        "- 1",
        "-\t1",
        "-\n1",
        "--1",
        "-",
        "+1",
        ".5",
        "-.5",
        "1.",
        "0.",
        "0.e1",
        "1.e1",
        "01",
        "-01",
        "00",
        "1e",
        "1e+",
        "1e-",
        "1_2",
        "1n",
        "NaN",
        "Infinity",
        "-Infinity",
        "1 2",
        "true false",
        "true\u0000",
        "1\u0000",
        "1.2\u0000",
        "\u00001",
        '"unterminated',
        '"bad\\x"',
        '"raw\nnewline"',
    ];
    for (const input of invalidInputs) {
        assertThrows(SyntaxError, () => JSON.rawJSON(input), "invalid input: " + JSON.stringify(input));
    }
    assertThrows(SyntaxError, () => JSON.rawJSON(), "missing arg throws SyntaxError");
})();

// 4. JSON.isRawJSON checks including Proxy, prototype inheritance, and cross-realm
(function testIsRawJSON() {
    assertSame(JSON.isRawJSON(), false, "no args");
    for (const nonRaw of [
        undefined, null, 0, 1, -0, NaN, true, false, "", "1",
        Symbol("1"), 1n, {}, [], { rawJSON: "1" }, () => {},
        Object.create(JSON.rawJSON("1")),
        new Proxy(JSON.rawJSON("1"), {}),
        new Number(1), new String("1"),
    ]) {
        assertSame(JSON.isRawJSON(nonRaw), false, "nonRaw");
    }

    const r = JSON.rawJSON("123");
    assertSame(JSON.isRawJSON(r), true, "rawJSON true");

    if (typeof WScript !== "undefined" && typeof WScript.LoadScript === "function") {
        const other = WScript.LoadScript(
            "function makeRaw(x) { return JSON.rawJSON(x); } " +
            "function checkRaw(x) { return JSON.isRawJSON(x); }",
            "samethread"
        );
        const crossRaw = other.makeRaw("9007199254740993");
        assertSame(JSON.isRawJSON(crossRaw), true, "cross-realm rawJSON detected locally");
        assertSame(other.checkRaw(r), true, "local rawJSON detected in other realm");
        assertSame(
            JSON.stringify({ x: crossRaw }),
            '{"x":9007199254740993}',
            "cross-realm rawJSON stringifies verbatim"
        );
    }
})();

// 5. JSON.stringify with rawJSON + LazyJSONString round-trip
(function testStringifyRawJSON() {
    assertSame(JSON.stringify(JSON.rawJSON("1.00")), "1.00", "root number");
    assertSame(JSON.stringify(JSON.rawJSON('"hello\\u0020world"')), '"hello\\u0020world"', "root string");
    assertSame(JSON.stringify(JSON.rawJSON("123456789012345678901234567890")), "123456789012345678901234567890", "root big");

    const obj = {
        a: JSON.rawJSON("1.00"),
        b: [JSON.rawJSON("true"), JSON.rawJSON('"\\u0041"'), JSON.rawJSON("null")],
        c: { d: JSON.rawJSON("1e1000") }
    };
    const compact = JSON.stringify(obj);
    assertSame(compact, '{"a":1.00,"b":[true,"\\u0041",null],"c":{"d":1e1000}}', "compact stringify");

    // Round-trip LazyJSONString via JSON.parse without reviver and with reviver
    const lazyCompact = JSON.stringify({ a: JSON.rawJSON("1.00"), big: JSON.rawJSON("1e1000") });
    const parsedNoReviver = JSON.parse(lazyCompact);
    assertSame(parsedNoReviver.a, 1, "lazy roundtrip no reviver a");
    assertSame(parsedNoReviver.big, Infinity, "lazy roundtrip no reviver big");

    const lazyForReviver = JSON.stringify({ a: JSON.rawJSON("1.00"), s: JSON.rawJSON('"\\u0041"') });
    const seenSources = {};
    const parsedWithReviver = JSON.parse(lazyForReviver, function (k, v, ctx) {
        if (k) {
            seenSources[k] = ctx.source;
        }
        return v;
    });
    assertSame(seenSources.a, "1.00", "lazy roundtrip reviver a source");
    assertSame(seenSources.s, '"\\u0041"', "lazy roundtrip reviver s source");
    assertSame(parsedWithReviver.s, "A", "lazy roundtrip reviver s value");

    // Indentation / space
    const spaced = JSON.stringify({ x: JSON.rawJSON("42"), y: [JSON.rawJSON("false")] }, null, 2);
    assertSame(spaced, '{\n  "x": 42,\n  "y": [\n    false\n  ]\n}', "spaced stringify");

    // Replacer function + property list + toJSON returning rawJSON
    const bigVal = BigInt(Number.MAX_SAFE_INTEGER) + 2n;
    assertSame(
        JSON.stringify({ n: bigVal }, (k, v) => (typeof v === "bigint" ? JSON.rawJSON(v) : v)),
        '{"n":9007199254740993}',
        "replacer bigint -> rawJSON"
    );
    assertSame(
        JSON.stringify({ a: JSON.rawJSON("1"), b: JSON.rawJSON("2") }, ["b"]),
        '{"b":2}',
        "replacer property list"
    );
    assertSame(
        JSON.stringify({ a: JSON.rawJSON("1") }, (k, v) => (k === "a" ? 99 : v)),
        '{"a":99}',
        "replacer replaces rawJSON"
    );
    assertSame(
        JSON.stringify({ toJSON() { return JSON.rawJSON("7.50"); } }),
        "7.50",
        "toJSON returning rawJSON"
    );

    // Proxy and Object.create(rawJSON) do NOT trigger rawJSON serialization
    assertSame(JSON.stringify(new Proxy(JSON.rawJSON("1"), {})), '{"rawJSON":"1"}', "proxy over rawJSON");
    assertSame(JSON.stringify(Object.create(JSON.rawJSON("1"))), "{}", "Object.create(rawJSON)");
})();

// 6. JSON.parse reviver 3rd-argument context & source
(function testParseReviverContext() {
    // Prototype setter on "source" must not be invoked
    Object.defineProperty(Object.prototype, "source", {
        set() {
            throw new Error("Object.prototype.source setter must not run");
        },
        configurable: true
    });
    try {
        const res = JSON.parse('"\\u0041"', function (k, v, ctx) {
            assertSame(arguments.length, 3, "reviver arguments.length");
            assertSame(k, "", "root key");
            assertSame(v, "A", "unescaped value");
            assertSame(Object.getPrototypeOf(ctx), Object.prototype, "ctx proto");
            assertArrayEqual(Object.getOwnPropertyNames(ctx), ["source"], "ctx props");
            assertArrayEqual(Object.getOwnPropertySymbols(ctx), [], "ctx symbols");
            const desc = Object.getOwnPropertyDescriptor(ctx, "source");
            assertSame(desc.value, '"\\u0041"', "raw string source");
            assertTrue(desc.writable && desc.enumerable && desc.configurable, "source desc attrs");
            return v;
        });
        assertSame(res, "A");
    } finally {
        delete Object.prototype.source;
    }

    // Numbers, booleans, null, -0, huge numbers, whitespace around JSON
    const primitives = [
        ["1", 1, "1"],
        [" 1.0\n", 1, "1.0"],
        ["-0", -0, "-0"],
        ["1e2", 100, "1e2"],
        ["1.1E+1", 11, "1.1E+1"],
        ["-1.1e-1", -0.11, "-1.1e-1"],
        ["1e1000", Infinity, "1e1000"],
        ["true", true, "true"],
        ["false", false, "false"],
        ["null", null, "null"],
        ['"\\n\\t"', "\n\t", '"\\n\\t"'],
    ];
    for (const [text, expectedVal, expectedSrc] of primitives) {
        let gotSrc;
        const val = JSON.parse(text, (k, v, ctx) => {
            gotSrc = ctx.source;
            return v;
        });
        assertSame(val, expectedVal, "primitive val for " + text);
        assertSame(gotSrc, expectedSrc, "primitive src for " + text);
    }

    // Objects & arrays have empty context {} with no own properties
    const log = [];
    JSON.parse('{"42": 1.0, "arr": [true, "x"], "obj": {}}', (k, v, ctx) => {
        gcNow();
        const names = Object.getOwnPropertyNames(ctx);
        const syms = Object.getOwnPropertySymbols(ctx);
        assertArrayEqual(syms, [], "no context symbols");
        if (typeof v === "object" && v !== null) {
            assertArrayEqual(names, [], "object/array context has no own props");
            log.push(k + ":obj");
        } else {
            assertArrayEqual(names, ["source"], "primitive context has source");
            log.push(k + ":" + ctx.source);
        }
        return v;
    });
    assertArrayEqual(log, ["42:1.0", "0:true", "1:\"x\"", "arr:obj", "obj:obj", ":obj"], "walk order");

    // Duplicate keys: last token wins
    let dupSrc;
    const dupObj = JSON.parse('{"a": 1, "a": 2.0, "0": 3, "0": "four"}', (k, v, ctx) => {
        if (k === "a") {
            dupSrc = ctx.source;
        }
        return v;
    });
    assertSame(dupObj.a, 2, "duplicate key val");
    assertSame(dupSrc, "2.0", "duplicate key last source wins");

    // Forward modification: push, add property, replace primitive, SameValue(-0 vs +0), replace object
    const fwdLog = [];
    const parsedFwd = JSON.parse(
        '{"a": 1, "b": 2, "c": -0, "arr": [10], "sub": {"x": 5}, "rep": {"y": 6}}',
        function (k, v, ctx) {
            gcNow();
            if (k === "a") {
                this.b = 99;
                this.c = 0; // +0 != -0 under SameValue
                this.arr.push(20);
                this.sub.added = 7;
                this.rep = { y: 6 }; // different object identity
            }
            fwdLog.push(k + "=" + ctx.source);
            return v;
        }
    );
    assertSame(parsedFwd.b, 99);
    assertSame(parsedFwd.c, 0);
    assertArrayEqual(
        fwdLog,
        [
            "a=1",
            "b=undefined",
            "c=undefined",
            "0=10",
            "1=undefined",
            "arr=undefined",
            "x=5",
            "added=undefined",
            "sub=undefined",
            "y=undefined",
            "rep=undefined",
            "=undefined",
        ],
        "forward modification sources"
    );

    // Substring & concat string input with GC
    const subJson = ("xx" + '{"k": 1.25, "s": "\\u0042"}' + "yy").slice(2, -2);
    const subSources = {};
    JSON.parse(subJson, (k, v, ctx) => {
        gcNow();
        if (k) subSources[k] = ctx.source;
        return v;
    });
    assertSame(subSources.k, "1.25", "substring input k");
    assertSame(subSources.s, '"\\u0042"', "substring input s");

    // Disallow whitespace after minus in JSON.parse
    assertThrows(SyntaxError, () => JSON.parse("- 1"), "JSON.parse('- 1')");
    assertThrows(SyntaxError, () => JSON.parse("- 1", (k, v) => v), "JSON.parse('- 1', reviver)");

    // Cross-realm reviver
    if (typeof WScript !== "undefined" && typeof WScript.LoadScript === "function") {
        const other = WScript.LoadScript(
            "function rev(k, v, ctx) { return k === 'n' ? ctx.source : v; }",
            "samethread"
        );
        const crossParsed = JSON.parse('{"n": 1.500}', other.rev);
        assertSame(crossParsed.n, "1.500", "cross-realm reviver receives source");
    }
})();

print("PASS");
