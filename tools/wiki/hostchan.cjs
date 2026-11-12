/* hostchan.cjs -- node's channel to the wiki's simulation host (`wikitest serve`, tagpu/ddraw/tests/wikihost.c):
   a request line written and its answer line read synchronously, on the caller's own thread, so the drop-in
   (mockup/simhost.js) answers as a function call does. CommonJS, so that module can require it.

   open(target) builds the target through wikibuild.mjs (host, asan or wine), makes two FIFOs in a private folder,
   opens both read-write -- so neither open waits for the other end -- and starts the host behind a shell that writes
   THE DEAD MARKER when it ends: `{"error":"the host exited N","dead":true}` and a newline, with no newline before
   it. A host that dies mid-request therefore answers with the marker instead of leaving the read waiting, and the
   channel refuses every request after it. A host that dies mid-answer has left part of a line in the answer FIFO
   (its stdout, a pipe, is fully buffered, and an abort or an exit flushes what the buffer held), and the marker
   lands at that line's end: so any line that ENDS in the marker is the host's death, never an answer. No answer
   ends so: the host's own "error" member opens an error answer and is never followed by `"dead":true`, and every
   quote inside a string it writes is escaped, so no string of an answer can spell the marker either.
   The host and its shell are one process group, killed when this process exits; the host also ends by itself on
   the end of its input, which this process's exit closes. Linux only, as every wiki check is.

   THE INSTANCE (tests/wikihost.c): each host is started with an id of 128 random bits, `instance`, which its hello
   answers and the drop-in stamps on every call, so a handle is only ever read by the host that gave it.

   tee(targets) opens one channel per target, all with one instance id (the builds must answer alike, ids
   included), and sends each request to all of them: the first's answer is the answer, and any other whose text
   differs is recorded in `differences` with its first differing path, or whole when either answer is no JSON
   (never thrown), so a run lists every request that differs. A request a build could not answer -- its host
   ended -- is recorded there too before it throws, so a caller that takes the throw for a refusal still finds it.
   The list is the module's, every tee's in one, so that a check reads it without holding the tee the drop-in
   made. */
'use strict';
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawn, spawnSync } = require('node:child_process');

const ROOT = path.resolve(__dirname, '../..');
const open_ = [];
const differences = [];

process.on('exit', () => {
  for (const c of open_) c.close();
});

function wineEnv() {
  const r = spawnSync('git', ['rev-parse', '--git-common-dir'], { cwd: ROOT, encoding: 'utf8' });
  const env = { ...process.env };
  env.WINEPREFIX ??= path.join(path.resolve(ROOT, r.stdout.trim()), '..', 'wineprefix');
  env.WINEDEBUG ??= '-all';
  return env;
}

// a host's instance id: 128 bits from the operating system's generator, drawn for each host started
const newInstance = () => crypto.randomBytes(16).toString('hex');
// THE DEAD MARKER at a line's end, the host's exit in it
const DEAD = /\{"error":"(the host exited -?\d+)","dead":true\}$/;

/* the channel to one host of `target`, its working folder the repository's root (the packs' paths are relative to
   it), its instance id `instance` */
function open(target, instance = newInstance()) {
  const { build } = require('./wikibuild.mjs');
  const exe = build(target);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'wikihost-'));
  const req = path.join(dir, 'req'), ans = path.join(dir, 'ans');
  const mk = spawnSync('mkfifo', [req, ans]);
  if (mk.status !== 0) throw new Error(`hostchan: mkfifo failed in ${dir}`);
  const wfd = fs.openSync(req, 'r+'), rfd = fs.openSync(ans, 'r+');
  const argv = target === 'wine' ? ['wine', exe] : [exe];
  // the shell waits for the host and then answers for it; `1<>` opens the answer FIFO without waiting for a reader
  const p = spawn('sh', ['-c', 'r=$1 a=$2 i=$3; shift 3; "$@" serve --instance "$i" <"$r" >"$a"; printf \'{"error":"the host exited %s","dead":true}\\n\' "$?" 1<>"$a"',
    'wikihost', req, ans, instance, ...argv], { cwd: ROOT, stdio: ['ignore', 'ignore', 'inherit'], detached: true, env: target === 'wine' ? wineEnv() : process.env });
  p.unref();
  const buf = Buffer.alloc(1 << 16);
  let rest = Buffer.alloc(0), dead = null, closed = false;
  const chan = {
    target,
    instance,
    pid: p.pid,                     // the wrapping shell's, whose child the host is
    /* the request line `line` (no newline in it) written; its answer is receive()'s */
    send(line) {
      if (dead) throw new Error(`hostchan ${target}: ${dead}`);
      const out = Buffer.from(`${line}\n`);
      for (let o = 0; o < out.length;) o += fs.writeSync(wfd, out, o, out.length - o);
    },
    /* the next answer line; throws once the host has ended */
    receive() {
      if (dead) throw new Error(`hostchan ${target}: ${dead}`);
      const chunks = [rest];
      let nl = rest.indexOf(10), seen = rest.length;
      while (nl < 0) {
        const n = fs.readSync(rfd, buf, 0, buf.length, null), c = Buffer.from(buf.subarray(0, n));
        const k = c.indexOf(10);
        chunks.push(c);
        if (k >= 0) nl = seen + k;
        seen += n;
      }
      const all = Buffer.concat(chunks), got = all.toString('utf8', 0, nl);
      rest = all.subarray(nl + 1);
      const m = DEAD.exec(got);
      if (m) {
        dead = m[1];
        throw new Error(`hostchan ${target}: ${dead}`);
      }
      return got;
    },
    request(line) { chan.send(line); return chan.receive(); },
    close() {
      if (closed) return;
      closed = true;
      try { process.kill(-p.pid, 'SIGKILL'); } catch (e) { /* already gone */ }
      for (const fd of [wfd, rfd]) try { fs.closeSync(fd); } catch (e) { /* already closed */ }
      fs.rmSync(dir, { recursive: true, force: true });
    },
  };
  open_.push(chan);
  return chan;
}

/* the first differing path between two parsed answers, or null */
function firstDiff(a, b, at = '') {
  if (Object.is(a, b)) return null;
  if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null || Array.isArray(a) !== Array.isArray(b))
    return { at: at || '(the answer)', a, b };
  const keys = [...new Set([...Object.keys(a), ...Object.keys(b)])];
  for (const k of keys) {
    const d = firstDiff(a[k], b[k], Array.isArray(a) ? `${at}[${k}]` : `${at}.${k}`);
    if (d) return d;
  }
  return null;
}

// an answer parsed, or its text when it is no JSON (a string, so it differs from any parsed object)
const parsed = t => { try { return JSON.parse(t); } catch (e) { return t; } };

function tee(targets) {
  const instance = newInstance(), chans = targets.map(t => open(t, instance));
  const short = line => (line.length > 300 ? `${line.slice(0, 297)}...` : line);
  return {
    target: targets.join('+'),
    instance,
    differences,
    request(line) {
      let got;
      try {
        for (const c of chans) c.send(line);
        got = chans.map(c => c.receive());
      } catch (e) {
        differences.push({ request: short(line), dead: e.message });
        throw e;
      }
      for (let k = 1; k < got.length; k++)
        if (got[k] !== got[0])
          differences.push({ request: short(line), target: chans[k].target,
            ...(firstDiff(parsed(got[0]), parsed(got[k])) || { at: '(the answer)', a: got[0], b: got[k] }) });
      return got[0];
    },
    close() { for (const c of chans) c.close(); },
  };
}

module.exports = { open, tee, firstDiff, differences };
