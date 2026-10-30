// Churn driver for the agent-startup SIGBUS lane (adapted from the S6e
// reviewer's rvb.mjs). Params: <bin> <killms> <agents> <count> <par> [interp]
import { spawn } from 'node:child_process';
import { writeFileSync } from 'node:fs';

const DIR = '/Users/ascotti/Documents/code/ChakraCore-wt/agent-startup-sigbus/out/sigbus';
const CH = { pre: DIR + '/bin-pre/ch', post: DIR + '/bin-post/ch', predbg: DIR + '/bin-pre-dbg/ch', postdbg: DIR + '/bin-post-dbg/ch' }[process.argv[2]];
const KILLMS = parseInt(process.argv[3], 10);
const N = parseInt(process.argv[4], 10);
const COUNT = parseInt(process.argv[5], 10);
const PAR = parseInt(process.argv[6], 10);
const INTERP = process.argv[7] === 'interp' ? DIR + '/sigbus-trace.dylib' : null;
const JS = DIR + `/nq_${N}.js`;
writeFileSync(JS, `// no-quit churn, ${N} agents, main spins forever
var N = ${N};
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
`);
const env = { ...process.env };
if (INTERP) {
  env.DYLD_INSERT_LIBRARIES = INTERP;
  if (process.argv[2].endsWith('dbg')) {
    env.SIGBUS_TRACE_IMAGE = '/bin-pre-dbg/ch';
    env.SIGBUS_TRACE_PVMOFF = '0xa0aecb0';
    env.SIGBUS_TRACE_CTOFF = '0xa202878';
  }
}
let idx = 0; const res = { sigbus: 0, segv: 0, realfault66: 0, killed: 0, other: 0, clean0: 0 };
const bt = [];
function runOne() {
  return new Promise((resolve) => {
    const i = ++idx;
    if (i > COUNT) return resolve();
    const child = spawn(CH, ['-ESBigInt', '-ESSharedArrayBuffer', '-nonative', '-Test262', JS], { stdio: ['ignore', 'ignore', 'pipe'], env });
    let err = '';
    child.stderr.on('data', d => { err += d; if (err.length > 200000) err = err.slice(-200000); });
    const to = setTimeout(() => child.kill('SIGKILL'), KILLMS + 400);
    child.on('close', (code, signal) => {
      clearTimeout(to);
      if (signal === 'SIGBUS') res.sigbus++;
      else if (signal === 'SIGSEGV') res.segv++;
      else if (code === 66) { res.realfault66++; if (bt.length < 5) bt.push(err); }
      else if (signal === 'SIGKILL') res.killed++;
      else if (code === 0 || code === null) res.clean0++;
      else { res.other++; if (bt.length < 3) bt.push(`code ${code} ${signal}: ${err.slice(0, 300)}`); }
      resolve(runOne);
    });
    child.on('error', () => { clearTimeout(to); res.other++; resolve(runOne); });
  }).then(f => (f ? f() : undefined));
}
const t0 = Date.now();
await Promise.all(Array.from({ length: PAR }, () => runOne()));
console.log(JSON.stringify({ bin: process.argv[2], killms: KILLMS, agents: N, count: COUNT, par: PAR, interp: !!INTERP, wall_sec: ((Date.now() - t0) / 1000).toFixed(0), ...res }));
for (const b of bt) console.log('---- sample ----\n' + b);
