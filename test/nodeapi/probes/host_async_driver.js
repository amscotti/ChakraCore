//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
'use strict';

const assert = require('assert');
const addon = globalThis.__exports;

const mode = (typeof process !== 'undefined' && process.env)
  ? process.env.HOST_ASYNC_EXPECT_TIMEOUT : '';
if (mode === '1' || mode === 'ref') {
  addon.holdLoopRefForever();
} else if (mode === 'work') {
  addon.slowWorkTimeout();
} else if (mode === 'limit') {
  Promise.resolve().then(() => Promise.resolve().then(() => 1));
} else {
  let completed = false;
  const seen = [];

  Promise.all([
    addon.asyncDouble(21).then((v) => {
      assert.strictEqual(v, 42);
      return v;
    }),
    addon.asyncDouble(-7).then((v) => {
      assert.strictEqual(v, -14);
      return v;
    }),
    addon.runTsfnStream((n) => {
      seen.push(n);
    }, 8).then((done) => {
      assert.strictEqual(done, true);
      assert.deepStrictEqual(seen, [1, 2, 3, 4, 5, 6, 7, 8]);
      return true;
    }),
  ]).then((vals) => {
    assert.deepStrictEqual(vals, [42, -14, true]);
    completed = true;
    globalThis.__hostAsyncCompleted = true;
    print('HOST_ASYNC_JS: PASS');
  }).catch((err) => {
    print('HOST_ASYNC_JS: FAIL ' + (err && err.stack ? err.stack : err));
  });

  if (completed) {
    throw new Error('async work completed synchronously unexpectedly');
  }
}
