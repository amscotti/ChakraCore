// N5.4 gc-hook proof driver (via ch-nodeapi).
// Asserts the host `globalThis.gc()` hook exists and drives collection:
// 12 repeats x 20 externals each; after gc() polling (up to 10 GCs,
// the shim gcUntil pattern) the finalizer count must be exact with no
// duplicates and no misses.
//
// Drop pattern: externals are created inside a helper function frame
// whose locals die on return. (Script-scope `stash = null` does NOT
// drop in this engine -- verified: the array survives 10 GCs -- while
// fn-scope drops immediately. The addon's count() self-drains the
// finalizer queue on entry before reading.)
'use strict';

if (typeof globalThis.gc !== 'function') {
  throw new Error('gc hook missing (FAIL)');
}
const E = globalThis.__exports;
const REPEATS = 12;
const PER = 20;

function makeBatch() {
  let stash = [];
  for (let i = 0; i < PER; i++) stash.push(E.create());
}

let prev = E.count();
for (let r = 0; r < REPEATS; r++) {
  makeBatch();
  let now = prev;
  for (let attempt = 0; attempt < 10 && now - prev < PER; attempt++) {
    globalThis.gc();
    now = E.count();
    if (now - prev > PER) break; // overshoot: fail below
  }
  if (now - prev !== PER) {
    throw new Error('gc_hook repeat ' + r + ': count ' + prev + ' -> ' + now);
  }
  prev = now;
}
print('gc_hook: PASS final=' + prev + ' (want ' + REPEATS * PER + ')');
