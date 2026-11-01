// C2-JIT-1 warmed-caller battery (jit-reentrancy-fix lane).
//
// Drives ALL seven Set.prototype set methods from a >=1000-iteration loop so
// the CALLING function is JIT-compiled under default flags (deterministic at
// >=1000 per doc/c2-jit-test262.md) and under -forceNative/-off:fulljit, then
// asserts BOTH the result contents AND that the set-like record's user
// callbacks (has / keys / next / return) actually executed with the exact
// call counts the spec walk produces. Aborts (SIGABRT) or wrong values fail.
//
// Expected-value oracle: the interpreter (-nonative) run of this same script,
// which matches test262 (372/372, doc/c2-jit-test262.md).
//
// Usage: ch warmed-set-methods.js   -> prints "pass", exit 0

var ITERATIONS = 1200; // >=1000: deterministic tier-up under default flags

function fail(msg) {
    throw new Error("battery: " + msg);
}

function contents(set) {
    var out = [];
    set.forEach(function (v) { out.push(v); });
    return out.join(",");
}

// A Set Record (sec-setrecords) whose members log every observable call.
// values: the keys() iterator yields these; hasMember: Set-like membership.
function makeRecorder(values, hasMember, hooks) {
    hooks = hooks || {};
    var counts = { size: 0, has: 0, keys: 0, next: 0, return: 0 };
    var record = {
        counts: counts,
        get size() {
            counts.size++;
            return values.length;
        },
        has: function (v) {
            counts.has++;
            if (hooks.has) { hooks.has(v); }
            return hasMember(v);
        },
        keys: function () {
            counts.keys++;
            if (hooks.keys) { hooks.keys(); }
            var index = 0;
            return {
                next: function () {
                    counts.next++;
                    if (hooks.next) { hooks.next(index); }
                    if (index >= values.length) {
                        return { done: true };
                    }
                    return { done: false, value: values[index++] };
                },
                return: function () {
                    counts.return++;
                    if (hooks.return) { hooks.return(); }
                    return { done: true };
                }
            };
        }
    };
    return record;
}

function memberOf34(v) { return v === 3 || v === 4; }

// One full walk of the seven methods against a fresh recorder.
// Returns nothing; throws on any wrong value or wrong callback count.
function checkAllMethods(receiver, values, hasMember, expect) {
    // expect: { method -> {content?, bool?, counts: {has,keys,next,return}} }
    var methods = ["union", "intersection", "difference", "symmetricDifference",
                   "isSubsetOf", "isSupersetOf", "isDisjointFrom"];
    for (var i = 0; i < methods.length; i++) {
        var m = methods[i];
        var rec = makeRecorder(values, hasMember);
        var out = receiver[m](rec);
        var e = expect[m];
        if (e.bool !== undefined) {
            if (out !== e.bool) {
                fail(m + ": expected " + e.bool + " got " + out);
            }
        } else if (contents(out) !== e.content) {
            fail(m + ": expected [" + e.content + "] got [" + contents(out) + "]");
        }
        var c = rec.counts;
        var x = e.counts;
        if (c.size !== 1) { fail(m + ": size read " + c.size + "x, expected 1"); }
        if (c.has !== x.has) { fail(m + ": has called " + c.has + "x, expected " + x.has); }
        if (c.keys !== x.keys) { fail(m + ": keys called " + c.keys + "x, expected " + x.keys); }
        if (c.next !== x.next) { fail(m + ": next called " + c.next + "x, expected " + x.next); }
        if (c.return !== x.return) { fail(m + ": return called " + c.return + "x, expected " + x.return); }
    }
}

// A = new Set([1,2,3]) (size 3) vs record [3,4] (size 2): receiver LARGER,
// so intersection/difference take the keys path, isSubsetOf is an immediate
// false, isSupersetOf/isDisjointFrom iterate keys and early-exit on a hit
// (closing the iterator: return runs).
function largeReceiver() {
    var a = new Set([1, 2, 3]);
    checkAllMethods(a, [3, 4], memberOf34, {
        union:              { content: "1,2,3,4", counts: { has: 0, keys: 1, next: 3, return: 0 } },
        intersection:       { content: "3",      counts: { has: 0, keys: 1, next: 3, return: 0 } },
        difference:         { content: "1,2",    counts: { has: 0, keys: 1, next: 3, return: 0 } },
        symmetricDifference:{ content: "1,2,4",  counts: { has: 0, keys: 1, next: 3, return: 0 } },
        isSubsetOf:         { bool: false,       counts: { has: 0, keys: 0, next: 0, return: 0 } },
        isSupersetOf:       { bool: false,       counts: { has: 0, keys: 1, next: 2, return: 1 } },
        isDisjointFrom:     { bool: false,       counts: { has: 0, keys: 1, next: 1, return: 1 } }
    });
}

// A2 = new Set([1,2]) (size 2) vs record [3,4] (size 2): receiver NOT larger,
// so intersection/difference/isSubsetOf/isDisjointFrom walk the receiver and
// call record.has per entry; isSupersetOf still iterates keys and early-exits.
function smallReceiver() {
    var a2 = new Set([1, 2]);
    checkAllMethods(a2, [3, 4], memberOf34, {
        union:              { content: "1,2,3,4", counts: { has: 0, keys: 1, next: 3, return: 0 } },
        intersection:       { content: "",        counts: { has: 2, keys: 0, next: 0, return: 0 } },
        difference:         { content: "1,2",     counts: { has: 2, keys: 0, next: 0, return: 0 } },
        symmetricDifference:{ content: "1,2,3,4", counts: { has: 0, keys: 1, next: 3, return: 0 } },
        isSubsetOf:         { bool: false,        counts: { has: 1, keys: 0, next: 0, return: 0 } },
        isSupersetOf:       { bool: false,        counts: { has: 0, keys: 1, next: 1, return: 1 } },
        isDisjointFrom:     { bool: true,         counts: { has: 2, keys: 0, next: 0, return: 0 } }
    });
}

// The minimal repro shape: `other` is a REAL Set (no user callbacks at all).
function realSetArgument() {
    var a = new Set([1, 2, 3]);
    var b = new Set([3, 4]);
    if (contents(a.union(b)) !== "1,2,3,4") { fail("union(realSet)"); }
    if (contents(a.intersection(b)) !== "3") { fail("intersection(realSet)"); }
    if (contents(a.difference(b)) !== "1,2") { fail("difference(realSet)"); }
    if (contents(a.symmetricDifference(b)) !== "1,2,4") { fail("symmetricDifference(realSet)"); }
    if (a.isSupersetOf(new Set([1, 3])) !== true) { fail("isSupersetOf(realSet, true)"); }
    if (a.isSupersetOf(b) !== false) { fail("isSupersetOf(realSet, false)"); }
    if (a.isDisjointFrom(new Set([9])) !== true) { fail("isDisjointFrom(realSet)"); }
    if (new Set([8, 9]).isSubsetOf(new Set([7, 8, 9])) !== true) { fail("isSubsetOf(realSet)"); }
}

// -0 keys CanonicalizeSetMethodKey path: a -0 from the keys iterator must not
// double with +0 already in the receiver's copy.
function negZeroKeys() {
    var a = new Set([0]);
    var rec = makeRecorder([-0, 5], function (v) { return false; });
    var out = a.union(rec);
    if (out.size !== 2) { fail("union(-0): expected size 2 got " + out.size); }
    if (!out.has(0) || !out.has(5)) { fail("union(-0): canonicalized members"); }
}

// The warmed loop is TOP-LEVEL and invokes the per-shape helpers by name:
// under default flags the helpers tier up by invocation count
// (deterministic at >=1000, matching doc/c2-jit-test262.md's caller-mode
// matrix), under -mic:1 the top-level loop itself tiers, and under
// -forceNative everything is compiled up front. Every armed C++->JS
// transition (SetMethodCallHas, GetSetLikeKeysIterator, SetLikeKeysStep,
// CloseKeysIteratorNormalCompletion) therefore runs with JIT-compiled frames
// as the JS caller.
var sink = 0;
for (var i = 0; i < ITERATIONS; i++) {
    largeReceiver();
    smallReceiver();
    realSetArgument();
    negZeroKeys();
    sink += i & 1;
}

if (sink !== ITERATIONS / 2) { fail("loop did not run to completion"); }
print("pass");
