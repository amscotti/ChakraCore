// no-quit churn, 16 agents, main spins forever
var N = 16;
var sab = new SharedArrayBuffer(8 * 4);
var ia = new Int32Array(sab);
var src =
    '$262.agent.receiveBroadcast(function(sab2) {' +
    'var v = new Int32Array(sab2);' +
    'Atomics.waitAsync(v, 0, 0).value.then(function(){});' +
    'Atomics.waitAsync(v, 0, 0, 400).value.then(function(){});' +
    'Atomics.add(v, 4, 1);' +
    '});';
for (var a = 0; a < N; a++) $262.agent.start(src);
$262.agent.broadcast(sab);
for (var k = 0; k < 500000000; k++) { if (Atomics.load(ia, 4) === N) break; }
while (true) { }
WScript.Quit(0);
