//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Cross-module lexical TDZ: a read of a binding whose declaration has not
// been evaluated yet must throw ReferenceError, even across module
// boundaries (cycles, namespace reads, typeof). Slots are pre-filled with
// the undeclared-block-var sentinel (SourceTextModuleRecord::
// InitializeLocalExports); direct reads go through LdModuleSlot+ChkUndecl,
// namespace reads through ModuleNamespace::GetNSSlot. Evaluation order must
// match a conforming engine: the reader's body runs before the throw, and
// the not-yet-reached module body never runs.

var cycletdz_events = [];
function cycletdz_reset() { cycletdz_events = []; }

WScript.RegisterModuleSource("cycletdz_a.js",
    `import { b } from "cycletdz_b.js";
    cycletdz_events.push("A");
    export let a = "A" + b;
    cycletdz_events.push("A2");`);
WScript.RegisterModuleSource("cycletdz_b.js",
    `import { a } from "cycletdz_a.js";
    cycletdz_events.push("B");
    export let b = "B" + a;
    cycletdz_events.push("B2");`);

WScript.RegisterModuleSource("cycletdz_ns_x.js",
    `import "cycletdz_ns_reader.js";
    export let y = 2;
    cycletdz_events.push("NS-X");`);
WScript.RegisterModuleSource("cycletdz_ns_reader.js",
    `import * as ns from "cycletdz_ns_x.js";
    cycletdz_events.push("NS");
    cycletdz_events.push("ns.y=" + ns.y);`);

WScript.RegisterModuleSource("cycletdz_typeof_x.js",
    `import "cycletdz_typeof_reader.js";
    export let a = 5;
    cycletdz_events.push("Typeof-X");`);
WScript.RegisterModuleSource("cycletdz_typeof_reader.js",
    `import { a } from "cycletdz_typeof_x.js";
    cycletdz_events.push("Typeof");
    cycletdz_events.push("typeof a=" + typeof a);`);

WScript.RegisterModuleSource("cycletdz_ok_b.js",
    `cycletdz_events.push("OK-B");
    export let b = "B";`);
WScript.RegisterModuleSource("cycletdz_ok_a.js",
    `import { b } from "cycletdz_ok_b.js";
    cycletdz_events.push("OK-A");
    export let a = "A" + b;`);

function checkCycle(specifier, label, expectedEvents) {
    cycletdz_reset();
    return import(specifier).then(
        () => print("FAILED: " + label + " did not throw"),
        (e) => {
            var events = cycletdz_events.join(",");
            var ok = (e instanceof ReferenceError) && (events === expectedEvents);
            print(ok ? "pass" : ("FAILED: " + label + " err=" + e + " events=" + events));
        });
}

checkCycle("cycletdz_a.js", "cyclic import read", "B")
    .then(() => checkCycle("cycletdz_ns_x.js", "namespace TDZ read", "NS"))
    .then(() => checkCycle("cycletdz_typeof_x.js", "typeof TDZ read", "Typeof"))
    .then(() => {
        cycletdz_reset();
        return import("cycletdz_ok_a.js").then(
            () => {
                var events = cycletdz_events.join(",");
                print(events === "OK-B,OK-A" ? "pass" : ("FAILED: post-init order=" + events));
            },
            (e) => print("FAILED: post-init read threw " + e));
    });
