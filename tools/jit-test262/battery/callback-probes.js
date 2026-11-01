// Callback-builtin probe battery (jit-reentrancy-fix lane), rebuilt from the
// adversarial review's 42-probe sweep list (doc/c2-jit-test262.md review
// outcome). Every probe is a C++ builtin that re-enters JS (comparator,
// callback, reviver, trap, coercion...) driven from a warmed TOP-LEVEL loop,
// executed under -forceNative -off:simpleJit -bgJitDelay:0 (and the other
// modes via run.sh). The pre-existing arming (EntryForEach-style
// BEGIN_SAFE_REENTRANT_CALL / ExecuteImplicitCall) must keep every probe
// green: NO probe may abort, and the Set-methods/groupBy arming added by this
// lane must not perturb any of them.
//
// Expected-value oracle: the interpreter (-nonative) run of this same script.
//
// Usage: ch callback-probes.js   -> prints "pass", exit 0

// 1200 default: deterministic tier-up under every JIT mode. The -nonative
// oracle / slow-machine runs can lower it: `ch -args 30 -endargs ...`.
var ITERATIONS = WScript.Arguments.length > 0 ? parseInt(WScript.Arguments[0], 10) : 1200;
if (!(ITERATIONS > 0)) { ITERATIONS = 1200; }

var probes = 0;
function fail(msg) {
    throw new Error("probe battery: " + msg);
}
function check(cond, what) {
    if (!cond) { fail(what); }
}

// ---------------------------------------------------------------- arrays ---
function p_sortComparator() {
    var a = [5, 3, 1, 4, 2];
    a.sort(function (x, y) { return x - y; });
    check(a.join() === "1,2,3,4,5", "sort comparator");
}
function p_sortComparatorObjects() {
    var a = [{ v: 3 }, { v: 1 }, { v: 2 }];
    a.sort(function (x, y) { return x.v - y.v; });
    check(a[0].v === 1 && a[2].v === 3, "sort comparator objects");
}
function p_toSortedComparator() {
    var a = [3, 1, 2];
    var s = a.toSorted(function (x, y) { return y - x; });
    check(s.join() === "3,2,1" && a.join() === "3,1,2", "toSorted comparator");
}
function p_map() {
    check([1, 2, 3].map(function (v, i) { return v * 10 + i; }).join() === "10,21,32", "map");
}
function p_filter() {
    check([1, 2, 3, 4].filter(function (v) { return v % 2 === 0; }).join() === "2,4", "filter");
}
function p_forEach() {
    var t = 0;
    [1, 2, 3].forEach(function (v, i, arr) { t += v + i; check(arr.length === 3, "forEach arr"); });
    check(t === 9, "forEach sum");
}
function p_reduce() {
    check([1, 2, 3, 4].reduce(function (acc, v) { return acc + v; }, 100) === 110, "reduce");
}
function p_reduceRight() {
    check(["a", "b", "c"].reduceRight(function (acc, v) { return acc + v; }) === "cba", "reduceRight");
}
function p_findFindIndex() {
    var arr = [4, 5, 6];
    check(arr.find(function (v) { return v > 4; }) === 5, "find");
    check(arr.findIndex(function (v) { return v > 4; }) === 1, "findIndex");
}
function p_findLast() {
    var arr = [4, 5, 6];
    check(arr.findLast(function (v) { return v > 4; }) === 6, "findLast");
    check(arr.findLastIndex(function (v) { return v > 4; }) === 2, "findLastIndex");
}
function p_someEvery() {
    check([1, 2, 3].some(function (v) { return v === 2; }) === true, "some");
    check([1, 2, 3].every(function (v) { return v > 0; }) === true, "every");
}
function p_flatMap() {
    check([1, 2].flatMap(function (v) { return [v, v]; }).join() === "1,1,2,2", "flatMap");
}
function p_arrayFromMapFn() {
    check(Array.from([1, 2], function (v) { return v + 1; }).join() === "2,3", "Array.from mapFn");
}
function p_arrayFromIterable() {
    var it = { [Symbol.iterator]: function () { var i = 0; return { next: function () { return i < 3 ? { done: false, value: i++ } : { done: true }; } }; } };
    check(Array.from(it).join() === "0,1,2", "Array.from iterable");
}

// --------------------------------------------------------------- strings ---
function p_replaceFunction() {
    check("a1b2".replace(/\d/g, function (m) { return String(Number(m) + 1); }) === "a2b3",
          "replace with function");
}
function p_replaceAllFunction() {
    check("x-x-x".replaceAll("x", function (m) { return m + "!"; }) === "x!-x!-x!",
          "replaceAll with function");
}
function p_replaceSubstrFunction() {
    check("abc".replace("b", function () { return "B"; }) === "aBc", "replace substring with function");
}
function p_stringIterator() {
    var out = "";
    for (var c of "ab") { out += c; }
    check(out === "ab", "string for-of iterator");
}

// ---------------------------------------------------------- typed arrays ---
function p_taSortComparator() {
    var t = new Uint8Array([3, 1, 2]);
    t.sort(function (a, b) { return b - a; });
    check(t.join() === "3,2,1", "TypedArray sort comparator");
}
function p_taMapForEachFilter() {
    var t = new Int32Array([1, 2, 3]);
    var m = t.map(function (v) { return v * 2; });
    check(m.join() === "2,4,6", "TypedArray map");
    var sum = 0;
    t.forEach(function (v) { sum += v; });
    check(sum === 6, "TypedArray forEach");
    check(t.filter(function (v) { return v > 1; }).join() === "2,3", "TypedArray filter");
    check(t.reduce(function (a, v) { return a + v; }, 0) === 6, "TypedArray reduce");
    check(t.find(function (v) { return v === 2; }) === 2, "TypedArray find");
    check(t.some(function (v) { return v === 3; }) && !t.every(function (v) { return v === 3; }),
          "TypedArray some/every");
}
function p_taFromMapper() {
    check(Uint8Array.from([1, 2], function (v) { return v + 1; }).join() === "2,3", "TA.from mapper");
}
function p_taToSortedComparator() {
    check(new Int32Array([3, 1]).toSorted(function (a, b) { return a - b; }).join() === "1,3",
          "TypedArray toSorted");
}

// ------------------------------------------------------ iterator helpers ---
function p_iteratorMapFilter() {
    var it = [1, 2, 3].values().map(function (v) { return v + 1; }).filter(function (v) { return v > 2; });
    check(it.next().value === 3 && it.next().value === 4 && it.next().done === true,
          "iterator map/filter helpers");
}
function p_iteratorTakeDrop() {
    var it = [1, 2, 3, 4].values().drop(1).take(2);
    check(it.next().value === 2 && it.next().value === 3 && it.next().done === true,
          "iterator take/drop helpers");
}
function p_iteratorFlatMap() {
    var it = [1, 2].values().flatMap(function (v) { return [v, v * 10]; });
    check([...it].join() === "1,10,2,20", "iterator flatMap helper");
}
function p_iteratorReduce() {
    check([1, 2, 3].values().reduce(function (a, v) { return a + v; }) === 6, "iterator reduce helper");
}
function p_iteratorForEach() {
    var t = 0;
    [1, 2].values().forEach(function (v) { t += v; });
    check(t === 3, "iterator forEach helper");
}
function p_customIteratorProtocol() {
    var obj = {
        [Symbol.iterator]: function () { var i = 0; return { next: function () { return i < 2 ? { done: false, value: i++ * 5 } : { done: true }; } }; }
    };
    var sum = 0;
    for (var v of obj) { sum += v; }
    check(sum === 5, "custom iterator for-of");
}
function p_spreadCustomIterable() {
    var obj = { [Symbol.iterator]: function () { var i = 0; return { next: function () { return i < 2 ? { done: false, value: i++ } : { done: true }; } }; } };
    check(Math.max(...obj) === 1, "spread of custom iterable");
}

// ----------------------------------------------------------------- proxy ---
function p_proxyGetTrap() {
    var p = new Proxy({}, { get: function (t, k) { return k === "x" ? 42 : undefined; } });
    check(p.x === 42, "proxy get trap");
}
function p_proxyHasTrap() {
    var p = new Proxy({}, { has: function (t, k) { return k === "x"; } });
    check("x" in p && !("y" in p), "proxy has trap");
}
function p_proxySetTrap() {
    var p = new Proxy({}, { set: function (t, k, v) { t[k] = v * 2; return true; } });
    p.x = 21;
    check(p.x === 42, "proxy set trap");
}
function p_proxyApplyTrap() {
    var f = new Proxy(function (x) { return x + 1; }, { apply: function (t, thiz, args) { return t(args[0]) * 10; } });
    check(f(1) === 20, "proxy apply trap");
}

// ------------------------------------------------------------------ JSON ---
function p_jsonReviver() {
    var o = JSON.parse('{"a":1,"b":2}', function (k, v) { return typeof v === "number" ? v * 2 : v; });
    check(o.a === 2 && o.b === 4, "JSON.parse reviver");
}
function p_jsonReplacer() {
    check(JSON.stringify({ a: 1, b: 2 }, function (k, v) { return typeof v === "number" ? v + 1 : v; })
          === '{"a":2,"b":3}', "JSON.stringify replacer");
}
function p_jsonToJSON() {
    var o = { toJSON: function () { return "json!"; } };
    check(JSON.stringify({ x: o }) === '{"x":"json!"}', "toJSON");
}

// ------------------------------------------------------------------ misc ---
function p_dateToJSON() {
    var d = new Date(0);
    d.toJSON = function () { return "epoch"; };
    check(JSON.stringify({ d: d }) === '{"d":"epoch"}', "Date toJSON override");
}
function p_datePrimitive() {
    var d = new Date(0);
    check(typeof d.valueOf() === "number" && d.valueOf() === 0, "Date valueOf");
}
function p_getters() {
    var o = {};
    var reads = 0;
    Object.defineProperty(o, "g", { get: function () { reads++; return reads; } });
    check(o.g === 1 && o.g === 2, "getter invocation");
}
function p_valueOfToPrimitive() {
    var o = { valueOf: function () { return 7; } };
    check(o * 2 === 14, "valueOf coercion in arithmetic");
}
function p_toStringToPrimitive() {
    var o = { toString: function () { return "s"; } };
    check("" + o === "s", "toString coercion");
}
function p_mapForEach() {
    var m = new Map([[1, "a"], [2, "b"]]);
    var ks = [], vs = [];
    m.forEach(function (v, k) { ks.push(k); vs.push(v); });
    check(ks.join() === "1,2" && vs.join() === "a,b", "Map.prototype.forEach");
}
function p_setForEach() {
    var s = new Set([1, 2]);
    var t = 0;
    s.forEach(function (v) { t += v; });
    check(t === 3, "Set.prototype.forEach");
}
function p_setCtorCustomIterable() {
    var obj = { [Symbol.iterator]: function () { var i = 0; return { next: function () { return i < 2 ? { done: false, value: i++ + 1 } : { done: true }; } }; } };
    check(new Set(obj).size === 2, "Set constructor from custom iterable");
}
function p_mapCtorCustomIterable() {
    var pairs = { [Symbol.iterator]: function () { var i = 0; return { next: function () { if (i >= 2) { return { done: true }; } var v = i++; return { done: false, value: [v, v * 10] }; } }; } };
    var m = new Map(pairs);
    check(m.size === 2 && m.get(1) === 10, "Map constructor from custom iterable");
}
function p_promiseThen() {
    var out = 0;
    Promise.resolve(21).then(function (v) { out = v * 2; });
    check(out === 0, "Promise.then not yet run (microtask)");
    return out;
}
function p_boundFunction() {
    var f = function (a, b) { return a + b; }.bind(null, 40);
    check(f(2) === 42, "bound function");
}
function p_generatorDriven() {
    function* g() { yield 1; yield 2; }
    var it = g();
    check(it.next().value === 1 && it.next().value === 2 && it.next().done === true,
          "generator next");
}
function p_objectEntriesGetters() {
    var o = {};
    Object.defineProperty(o, "a", { get: function () { return 1; }, enumerable: true });
    Object.defineProperty(o, "b", { get: function () { return 2; }, enumerable: true });
    check(Object.values(o).join() === "1,2", "Object.values with getters");
    check(Object.entries(o).length === 2, "Object.entries with getters");
}
function p_arraySpeciesSubclass() {
    class Sub extends Array {}
    var s = new Sub(1, 2).map(function (v) { return v; });
    check(s instanceof Sub && s.join() === "1,2", "species subclass map");
}
function p_weakrefAndFinalization() {
    var wr = new WeakRef({});
    check(typeof wr.deref === "function", "WeakRef.deref callable");
    check(typeof FinalizationRegistry === "function", "FinalizationRegistry present");
}

// ------------------------------------------------------------------ run ----
var probeFns = [
    p_sortComparator, p_sortComparatorObjects, p_toSortedComparator, p_map, p_filter,
    p_forEach, p_reduce, p_reduceRight, p_findFindIndex, p_findLast, p_someEvery,
    p_flatMap, p_arrayFromMapFn, p_arrayFromIterable,
    p_replaceFunction, p_replaceAllFunction, p_replaceSubstrFunction, p_stringIterator,
    p_taSortComparator, p_taMapForEachFilter, p_taFromMapper, p_taToSortedComparator,
    p_iteratorMapFilter, p_iteratorTakeDrop, p_iteratorFlatMap, p_iteratorReduce,
    p_iteratorForEach, p_customIteratorProtocol, p_spreadCustomIterable,
    p_proxyGetTrap, p_proxyHasTrap, p_proxySetTrap, p_proxyApplyTrap,
    p_jsonReviver, p_jsonReplacer, p_jsonToJSON,
    p_dateToJSON, p_datePrimitive,
    p_getters, p_valueOfToPrimitive, p_toStringToPrimitive,
    p_mapForEach, p_setForEach, p_setCtorCustomIterable, p_mapCtorCustomIterable,
    p_promiseThen, p_boundFunction, p_generatorDriven,
    p_objectEntriesGetters, p_arraySpeciesSubclass, p_weakrefAndFinalization
];
probes = probeFns.length;

// Warmed top-level loop: each probe helper tiers up under default flags, the
// loop tiers under -mic:1, and -forceNative compiles everything up front.
for (var i = 0; i < ITERATIONS; i++) {
    for (var j = 0; j < probeFns.length; j++) {
        probeFns[j]();
    }
}
print("pass (" + probes + " probes x " + ITERATIONS + " iterations)");
