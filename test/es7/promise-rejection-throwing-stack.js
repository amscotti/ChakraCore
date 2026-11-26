//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Focused regression test for WScriptJsrt::PromiseRejectionTrackerCallback when
// accessing reason.stack, reason.stack.toString(), or reason.toString() throws.
// Must run with -TrackRejectedPromises without leaving a pending exception.

const throwingStackGetter = {
    get stack() {
        throw new Error("getter-boom");
    },
    toString() {
        return "fallback-after-stack-getter";
    }
};

const throwingStackToString = {
    stack: {
        toString() {
            throw new Error("stack-toString-boom");
        }
    },
    toString() {
        return "fallback-after-stack-toString";
    }
};

const throwingReasonToString = {
    toString() {
        throw new Error("reason-toString-boom");
    }
};

const nullProtoReason = Object.defineProperty(Object.create(null), "stack", {
    get() {
        throw new Error("nullproto-stack-boom");
    }
});
const symbolReason = Symbol("sym-reason");

const p1 = Promise.reject(throwingStackGetter);
const p2 = Promise.reject(throwingStackToString);
const p3 = Promise.reject(throwingReasonToString);
const p4 = Promise.reject(nullProtoReason);
const p5 = Promise.reject(symbolReason);

// Handle later to exercise both unhandled and handled tracker invocations.
p1.catch(() => {});
p2.catch(() => {});
p3.catch(() => {});
p4.catch(() => {});
p5.catch(() => {});

// Uncaught variants after handled ones to verify clean exception state across callbacks.
Promise.reject(throwingStackGetter);
Promise.reject(throwingStackToString);
Promise.reject(throwingReasonToString);
Promise.reject(nullProtoReason);
Promise.reject(symbolReason);

WScript.Echo("TRACKER_SURVIVED");
