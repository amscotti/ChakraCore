//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Focused regressions for GR-FE6 (statement-initial import() with operators)
// and GR-FE7 (sloppy `let` followed by newline and reserved-word statement/expr).

function assert(cond, msg) {
    if (!cond) {
        throw new Error("Assertion failed: " + msg);
    }
}

function assertParses(src, label) {
    try {
        new Function(src);
    } catch (e) {
        throw new Error("Expected valid syntax (" + label + "): " + JSON.stringify(src) + " -> " + e);
    }
    // Also exercise deferred parsing (inner function body).
    try {
        new Function("function inner() {\n" + src + "\n}");
    } catch (e) {
        throw new Error("Expected valid deferred syntax (" + label + "): " + JSON.stringify(src) + " -> " + e);
    }
}

function assertSyntaxError(src, label) {
    var threw = false;
    try {
        new Function(src);
    } catch (e) {
        if (e instanceof SyntaxError) {
            threw = true;
        } else {
            throw new Error("Expected SyntaxError (" + label + "), got: " + e);
        }
    }
    assert(threw, "Expected SyntaxError (" + label + "): " + JSON.stringify(src));
}

// --- GR-FE6: Statement-initial import() followed by operators ---
var importValid = [
    'import("x") + 1;',
    'import("x") - 1;',
    'import("x") * 2;',
    'import("x") == 1;',
    'import("x") && f();',
    'import("x") || f();',
    'import("x") ?? f();',
    'import("x") ? 1 : 2;',
    'import("x"), 1;',
    'import("x") in o;',
    'import("x") instanceof Object;',
    'import("x").then(f) + 1;',
    'import("x")[0] + 1;',
    'lbl: import("x") + 1;',
    'if (0) import("x") + 1;'
];
for (var i = 0; i < importValid.length; i++) {
    assertParses(importValid[i], "GR-FE6 valid #" + i);
}

var importInvalid = [
    'import("x") = 1;',
    '++import("x");',
    'import("x")++;',
    'import("x") += 1;'
];
for (var j = 0; j < importInvalid.length; j++) {
    assertSyntaxError(importInvalid[j], "GR-FE6 invalid #" + j);
}

// --- GR-FE7: Sloppy `let` as identifier followed by newline + keyword ---
var letValid = [
    'let\nif (0) ;',
    'let\nwhile (0) ;',
    'let\ndo {} while (0);',
    'let\nfor (;;) break;',
    'let\nfunction f() {}',
    'let\nclass C {}',
    'let\nreturn;',
    'let\nvoid 0;',
    'let\ntypeof x;',
    'let\nnew x();',
    'let\nswitch (0) {}',
    'let\ntry {} catch (e) {}',
    'let\nvar x = 1;',
    'let\nthrow 1;',
    'let\ndebugger;',
    'let\nconst x = 1;',
    'let\ntrue;',
    'let\nfalse;',
    'let\nnull;',
    'let\nthis;',
    'let\nin x;',
    'let in x;',
    'let\ninstanceof x;',
    'let instanceof x;',
    'if (0) let\nif (0) ;',
    'function* g() { let\nyield 1; }',
    'async function f() { let\nawait 1; }',
    'for (let in {}) ;',
    'for (let\nin {}) ;',
    // Controls that remain valid declarations:
    'let\nx = 1;',
    'let\n[x] = [1];',
    'let\n{x} = {x: 1};',
    'let\nyield = 1;',
    'let\nawait = 1;'
];
for (var k = 0; k < letValid.length; k++) {
    assertParses(letValid[k], "GR-FE7 valid #" + k);
}

// Verify runtime behavior when `let` is bound as a sloppy var:
var fn = new Function('var let = 7; var hit = false; let\nif (let === 7) hit = true; return hit;');
assert(fn() === true, "GR-FE7 runtime execution with sloppy var let");

var letInvalid = [
    'let if = 1;',
    'let\nlet = 1;',
    'let\nlet x = 1;',
    '"use strict"; let\nif (0) ;',
    '"use strict"; let\nwhile (0) ;',
    'if (0) let\n[x] = 0;',
    'if (0) let x = 1;'
];
for (var m = 0; m < letInvalid.length; m++) {
    assertSyntaxError(letInvalid[m], "GR-FE7 invalid #" + m);
}

print("PASS");
