//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Verify ES AsyncGeneratorYield awaits yielded values inside the generator
// execution context so throwing thenables / rejected promises enter try/catch
// and try/finally blocks rather than directly rejecting the outer promise.

var failures = [];

function check(cond, msg) {
    if (!cond) {
        failures.push(msg);
        print("async-generator-yield-throwing-thenable FAIL: " + msg);
    }
}

var promises = [];

function addCase(name, fn) {
    promises.push(fn().catch(function (err) {
        check(false, name + " threw unexpectedly: " + (err && err.stack ? err.stack : err));
    }));
}

addCase("yield throwing thenable getter inside try/catch", async function () {
    var log = [];
    async function* agf() {
        try {
            yield {
                get then() {
                    throw new Error("boom");
                }
            };
            log.push("after-yield");
        } catch (e) {
            log.push("caught:" + e.message);
            return "recovered";
        }
    }

    var g = agf();
    var r1 = await g.next();
    var r2 = await g.next();
    check(log.join(",") === "caught:boom", "log was " + log.join(","));
    check(r1.value === "recovered" && r1.done === true, "r1=" + JSON.stringify(r1));
    check(r2.value === undefined && r2.done === true, "r2=" + JSON.stringify(r2));
});

addCase("yield throwing thenable method inside try/catch with recovery yield", async function () {
    var log = [];
    async function* agf() {
        yield "before";
        try {
            yield {
                then: function () {
                    throw new Error("then-call-boom");
                }
            };
            log.push("unreachable");
        } catch (e) {
            log.push("caught:" + e.message);
            yield "fallback";
        } finally {
            log.push("finally");
        }
        return "end";
    }

    var g = agf();
    var r1 = await g.next();
    var r2 = await g.next();
    var r3 = await g.next();
    check(r1.value === "before" && r1.done === false, "r1");
    check(r2.value === "fallback" && r2.done === false, "r2");
    check(r3.value === "end" && r3.done === true, "r3");
    check(log.join(",") === "caught:then-call-boom,finally", "log=" + log.join(","));
});

addCase("yield throwing thenable runs finally before rejecting", async function () {
    var finallyRan = false;
    async function* agf() {
        try {
            yield {
                get then() {
                    throw new Error("finally-boom");
                }
            };
        } finally {
            finallyRan = true;
        }
    }

    var g = agf();
    var rejected = false;
    try {
        await g.next();
    } catch (e) {
        rejected = (e && e.message === "finally-boom");
    }
    check(finallyRan, "finally block must run");
    check(rejected, "outer promise must reject with finally-boom");
    var after = await g.next();
    check(after.done === true && after.value === undefined, "generator completed after uncaught throw");
});

addCase("yield rejected promise inside try/catch", async function () {
    async function* agf() {
        try {
            yield Promise.reject("rej-reason");
            return "unreachable";
        } catch (e) {
            return "caught:" + e;
        }
    }

    var r = await agf().next();
    check(r.value === "caught:rej-reason" && r.done === true, "rejected promise caught");
});

addCase("yield promise with throwing constructor getter inside try/catch", async function () {
    async function* agf() {
        var p = Promise.resolve(1);
        Object.defineProperty(p, "constructor", {
            get: function () {
                throw new Error("ctor-boom");
            }
        });
        try {
            yield p;
            return "unreachable";
        } catch (e) {
            return "caught:" + e.message;
        }
    }

    var r = await agf().next();
    check(r.value === "caught:ctor-boom" && r.done === true, "sync InternalPromiseResolve throw caught");
});

addCase("yield* sync iterable with throwing thenable inside try/catch", async function () {
    var closed = false;
    var iterable = {};
    iterable[Symbol.iterator] = function () {
        var step = 0;
        return {
            next: function () {
                if (step++ === 0) {
                    return {
                        value: {
                            get then() {
                                throw new Error("star-boom");
                            }
                        },
                        done: false
                    };
                }
                return { value: undefined, done: true };
            },
            return: function () {
                closed = true;
                return { value: undefined, done: true };
            }
        };
    };

    async function* agf() {
        try {
            yield* iterable;
            return "unreachable";
        } catch (e) {
            return "caught-star:" + (e instanceof TypeError ? "TypeError" : e.message);
        }
    }

    var r = await agf().next();
    check(closed, "inner iterator closed");
    check(r.value === "caught-star:star-boom" && r.done === true, "r=" + JSON.stringify(r));
});

addCase("return throwing thenable on completed vs suspended generator", async function () {
    async function* completedAgf() {}
    var g1 = completedAgf();
    await g1.next();
    var completedRejected = false;
    try {
        await g1.return({
            get then() {
                throw new Error("completed-return-boom");
            }
        });
    } catch (e) {
        completedRejected = (e && e.message === "completed-return-boom");
    }
    check(completedRejected, "return(throwingThenable) on completed generator must reject");

    async function* suspendedAgf() {
        try {
            yield 1;
        } catch (e) {
            return "caught-return:" + e.message;
        }
    }
    var g2 = suspendedAgf();
    var first = await g2.next();
    check(first.value === 1 && first.done === false, "suspended first yield");
    var ret = await g2.return({
        get then() {
            throw new Error("suspended-return-boom");
        }
    });
    check(ret.value === "caught-return:suspended-return-boom" && ret.done === true,
        "return(throwingThenable) on suspended generator must throw into try/catch");
});

Promise.all(promises).then(function () {
    if (failures.length > 0) {
        WScript.Quit(1);
    }
    print("pass");
}, function (err) {
    print("async-generator-yield-throwing-thenable UNHANDLED: " + err);
    WScript.Quit(1);
});
