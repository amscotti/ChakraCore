//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Regression test for FlowGraph::Dominates / VerifyLoopGraph() on labeled
// breaks from inner finally blocks to an outer label at the same EH depth,
// plus outer early exits and loop-carried breaks across finally blocks.

function nestedBreakInsideOuterTry() {
    var log = [];
    try {
        L: {
            try {
                try {
                    throw 1;
                } finally {
                    break L;
                }
            } catch (e) {
                log.push("catch");
            }
        }
        log.push("after");
    } finally {
        log.push("outer");
    }
    return log.join(",");
}

function nestedBreakOutOfOuterTry() {
    var log = [];
    L: {
        try {
            try {
                throw 1;
            } finally {
                log.push("inner");
                break L;
            }
        } finally {
            log.push("outer");
        }
        log.push("unreachable");
    }
    log.push("done");
    return log.join(",");
}

function loopWithInnerFinallySameDepthBreak() {
    var log = [];
    for (var i = 0; i < 3; i++) {
        try {
            L: {
                try {
                    if (i === 1) {
                        break L;
                    }
                    log.push("try" + i);
                } finally {
                    if (i === 2) {
                        break L;
                    }
                    log.push("fin" + i);
                }
                log.push("body" + i);
            }
            log.push("after" + i);
        } finally {
            log.push("outer" + i);
        }
    }
    return log.join(",");
}

for (var i = 0; i < 500; i++) {
    var r1 = nestedBreakInsideOuterTry();
    if (r1 !== "after,outer") {
        throw new Error("nestedBreakInsideOuterTry mismatch: " + r1);
    }
    var r2 = nestedBreakOutOfOuterTry();
    if (r2 !== "inner,outer,done") {
        throw new Error("nestedBreakOutOfOuterTry mismatch: " + r2);
    }
    var r3 = loopWithInnerFinallySameDepthBreak();
    var exp3 = "try0,fin0,body0,after0,outer0,fin1,after1,outer1,try2,after2,outer2";
    if (r3 !== exp3) {
        throw new Error("loopWithInnerFinallySameDepthBreak mismatch: " + r3);
    }
}

print("PASS");
