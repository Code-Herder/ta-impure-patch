/* cobhost.mjs -- the in-game wiki's COB host for node: sim.js's ctx.cob, answered by tools/wiki/serve.py --stdio,
   synchronously.

   sim.js is synchronous, and its host must answer each call before the call returns, as the page's synchronous
   request to serve.py does. Node has no synchronous read of a child's pipe, so a worker thread owns the child: the
   caller writes the request into a shared buffer, wakes the worker and waits on an Atomics.wait; the worker writes
   the request to serve.py, reads its answer line and wakes the caller. One request is in flight at a time.

     import { cobHost } from './cobhost.mjs';
     const cob = cobHost();                   // starts serve.py --stdio once
     WikiSim.play(entry, demo, layout, { ...ctx, cob: cob.forPack('tools/wiki/mockup/gfx/quickbuild') });
     cob.close();

   forPack(pack) is the object sim.js takes as ctx.cob (research/notes/ingame-wiki-cob-plan.md, *The wiki host*): its
   open(names) makes a host for those type names, each script read from that gfx pack, and returns the world, whose
   calls answer the host's state. */
import path from 'node:path';
import { Worker } from 'node:worker_threads';
import { createRequire } from 'node:module';

const { cobWorld } = createRequire(import.meta.url)('./mockup/cob.js');

const ROOT = new URL('../../', import.meta.url).pathname;
const CAP = 1 << 24;                  // one request or answer, in bytes

const WORKER = `
const { workerData, parentPort } = require('node:worker_threads');
const { spawn } = require('node:child_process');
const { ctl, buf, root } = workerData;
const flag = new Int32Array(ctl), bytes = new Uint8Array(buf);
const child = spawn('python3', [root + 'tools/wiki/serve.py', '--stdio'], { stdio: ['pipe', 'pipe', 'inherit'] });
let pending = '';
child.stdout.setEncoding('utf8');
child.stdout.on('data', d => {
  pending += d;
  const nl = pending.indexOf('\\n');
  if (nl < 0) return;
  const line = Buffer.from(pending.slice(0, nl), 'utf8');
  pending = pending.slice(nl + 1);
  if (line.length > bytes.length) { flag[1] = -1; } else { bytes.set(line); flag[1] = line.length; }
  Atomics.store(flag, 0, 2);
  Atomics.notify(flag, 0);
});
child.on('exit', code => { flag[1] = -2; Atomics.store(flag, 0, 2); Atomics.notify(flag, 0); });
parentPort.on('message', m => {
  if (m === 'close') { child.stdin.end(); return; }
  const n = flag[1];
  child.stdin.write(Buffer.from(bytes.subarray(0, n)).toString('utf8') + '\\n');
});
`;

export function cobHost() {
  const ctl = new SharedArrayBuffer(8), buf = new SharedArrayBuffer(CAP);
  const flag = new Int32Array(ctl), bytes = new Uint8Array(buf);
  const worker = new Worker(WORKER, { eval: true, workerData: { ctl, buf, root: ROOT } });
  worker.unref();
  const enc = new TextEncoder(), dec = new TextDecoder();
  // one synchronous request: the answer object, or throws with serve.py's error
  const request = req => {
    const b = enc.encode(JSON.stringify(req));
    if (b.length > CAP) throw new Error('cobhost: the request is too large');
    bytes.set(b);
    flag[1] = b.length;
    Atomics.store(flag, 0, 1);
    worker.postMessage('go');
    Atomics.wait(flag, 0, 1);
    const n = flag[1];
    if (n === -2) throw new Error('cobhost: serve.py --stdio exited');
    if (n < 0) throw new Error('cobhost: the answer is too large');
    const got = JSON.parse(dec.decode(bytes.slice(0, n)));
    if (got.error) throw new Error(`cobhost: ${got.error}`);
    return got;
  };
  return {
    request,
    forPack: pack => ({ open: names => cobWorld(request, names.map(n => n && { name: n, pack: path.relative(ROOT, path.resolve(ROOT, pack)) })) }),
    close: () => { worker.postMessage('close'); },
  };
}
