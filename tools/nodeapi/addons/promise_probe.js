// Driver for the promise-pump probe. Relies on the host pumping the
// JsSetPromiseContinuationCallback queue after JsRun (PR-8). Without
// pumping, the .then chain never runs and no output is produced.
let out = "";
Promise.resolve(1)
  .then(function (v) { out += v; return Promise.resolve(2); })
  .then(function (v) { out += v; print("promise: done:" + out); });
