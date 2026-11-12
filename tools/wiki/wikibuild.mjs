/* wikibuild.mjs -- the one recipe for the wiki's simulation host, tagpu/ddraw/tests/wikitest.c (`wikitest serve`,
   tests/wikihost.c): its sources and its three builds' flags live here and nowhere else.

     host   the mockup's host (tools/wiki/serve.py's /sim, the node checks through hostchan.cjs): gcc -O2. No -march,
            so no FMA, and -std=c99 keeps -ffp-contract=off: the doubles round as the DLL's SSE2 build rounds them.
     asan   the same under AddressSanitizer and UBSan, every finding fatal.
     wine   i686 as the DLL is built, the simulation's sources on SSE2 as the Makefile compiles them, run under Wine.
            __USE_MINGW_ANSI_STDIO makes its %.17g print as glibc's does (msvcrt writes three exponent digits), so
            the three builds' answers compare as text; it touches this test program only.

   build(target) returns the executable's path, built once per content: build/wikihost/<sha256 of the target, its
   flags and every source and header it can read>/wikitest[.exe]. A build compiles into a temporary of its own and
   publishes it by rename, under an flock(1) on build/wikihost/<target>.lock, so processes started together (the
   shards, serve.py, the node checks) build each target once and never run a half-written file; a builder that dies
   releases the lock with its process. CLI: `node tools/wiki/wikibuild.mjs host|asan|wine` prints the path and says on
   stderr when it compiled. */
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { TASIM_SRCS } from './tasimsrcs.mjs';

const ROOT = path.resolve(new URL('../..', import.meta.url).pathname);
const DDRAW = path.join(ROOT, 'tagpu/ddraw');
const CACHE = path.join(ROOT, 'build/wikihost');

/* the simulation (its front end and the core), the loader, the replay layer, the COB host, ImpAI's engine-free half
   (wikitest impai), and the stage with what
   it draws through (tagpu_wikistage_pose for the `poses` op) */
export const SOURCES = ['tests/wikitest.c', 'tests/wikihost.c', 'src/tagpu_wikisim.c', ...TASIM_SRCS, 'src/tagpu_wikidata.c',
  'src/tagpu_wikireplay.c', 'src/tagpu_wikistage.c', 'src/tagpu_wikiframe.c', 'src/tagpu_hudlay.c', 'src/tagpu_json.c',
  'src/tagpu_pak.c', 'src/tagpu_placegeo.c', 'src/tagpu_sweepgeo.c', 'src/tagpu_wikicob.c', 'src/tagpu_cobvm.c',
  'src/tagpu_cob_safe.c', 'src/impai_know.c', 'src/impai_maps.c'];
/* the sources the Makefile compiles on SSE2 (its CFLAGS += -msse2 lines) */
const SSE = new Set(['src/tagpu_wikisim.c', ...TASIM_SRCS, 'src/tagpu_wikidata.c', 'src/tagpu_wikicob.c', 'src/tagpu_sweepgeo.c',
  'src/tagpu_wikireplay.c', 'src/tagpu_wikistage.c', 'src/tagpu_wikiframe.c']);
const SSE_FLAGS = ['-msse2', '-mfpmath=sse', '-mincoming-stack-boundary=2'];

const TARGETS = {
  host: { cc: 'gcc', flags: ['-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Iinc'], libs: ['-lm'], exe: 'wikitest' },
  asan: { cc: 'gcc', flags: ['-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Iinc', '-fsanitize=address,undefined',
    '-fno-sanitize-recover=all'], libs: ['-lm'], exe: 'wikitest' },
  wine: { cc: 'i686-w64-mingw32-gcc', flags: ['-std=c99', '-O2', '-Wall', '-Iinc', '-Isrc', '-D__USE_MINGW_ANSI_STDIO=1'], libs: ['-static'],
    exe: 'wikitest.exe', sse: true },
};

/* every file a build reads: the listed sources and every header of src/, inc/ and tests/ */
function inputs() {
  const hs = ['src', 'inc', 'tests'].flatMap(d => fs.readdirSync(path.join(DDRAW, d), { recursive: true })
    .filter(f => /\.h$/.test(f)).map(f => `${d}/${f}`));
  return [...SOURCES, ...hs.sort()];
}

function digest(name) {
  const t = TARGETS[name], h = crypto.createHash('sha256');
  h.update(JSON.stringify([name, t]));
  for (const f of inputs()) h.update(`${f}\0`).update(fs.readFileSync(path.join(DDRAW, f))).update('\0');
  return h.digest('hex');
}

function run(cmd, args) {
  const r = spawnSync(cmd, args, { cwd: DDRAW, encoding: 'utf8', maxBuffer: 1 << 26 });
  if (r.error || r.status !== 0) throw new Error(`${cmd} ${args.join(' ')} failed${r.error ? `: ${r.error.message}` : ''}\n${r.stderr || ''}`);
}

/* compile `name` into `out` through a temporary of this process's own; the caller holds the lock */
function compile(name, out) {
  const t = TARGETS[name], tmp = `${out}.${process.pid}.tmp`;
  fs.mkdirSync(path.dirname(out), { recursive: true });
  process.stderr.write(`wikibuild: compiling ${name}\n`);
  try {
    if (t.sse) {
      const objdir = `${tmp}.o`;
      fs.mkdirSync(objdir, { recursive: true });
      const objs = [...SSE].map(src => {
        const o = path.join(objdir, `${path.basename(src, '.c')}.o`);
        run(t.cc, [...t.flags, ...SSE_FLAGS, '-c', '-o', o, src]);
        return o;
      });
      run(t.cc, [...t.flags, '-o', tmp, ...SOURCES.filter(s => !SSE.has(s)), ...objs, ...t.libs]);
      fs.rmSync(objdir, { recursive: true, force: true });
    } else run(t.cc, [...t.flags, '-o', tmp, ...SOURCES, ...t.libs]);
    fs.renameSync(tmp, out);
  } catch (e) {
    fs.rmSync(tmp, { force: true });
    fs.rmSync(`${tmp}.o`, { recursive: true, force: true });
    throw e;
  }
}

/* the executable of target `name` (host, asan or wine), built when this content has none yet */
export function build(name) {
  if (!TARGETS[name]) throw new Error(`wikibuild: no target ${name} (host, asan or wine)`);
  const out = path.join(CACHE, digest(name), TARGETS[name].exe);
  if (fs.existsSync(out)) return out;
  fs.mkdirSync(CACHE, { recursive: true });
  // the lock's holder re-checks the cache: whoever waited behind the first builder finds its file and compiles nothing
  const r = spawnSync('flock', [path.join(CACHE, `${name}.lock`), process.execPath, new URL(import.meta.url).pathname, '--locked', name, out],
    { stdio: ['ignore', 'ignore', 'inherit'] });
  if (r.error || r.status !== 0 || !fs.existsSync(out)) throw new Error(`wikibuild: building ${name} failed${r.error ? `: ${r.error.message}` : ''}`);
  return out;
}

/* the environment the `wine` target runs in: WINEPREFIX the prefix tools/wiki/wineprefix.py gives -- $WINEPREFIX
   checked against the template's shared hives, else the repository's own prefix, made the first time (its
   THE INVARIANT) -- and Wine's debug output off. Asked once a process; throws, saying why, when it cannot be had. */
let wineEnvOnce = null;
export function wineEnv() {
  if (wineEnvOnce) return wineEnvOnce;
  const r = spawnSync('python3', [path.join(ROOT, 'tools/wiki/wineprefix.py')], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  if (r.error || r.status !== 0)
    throw new Error(`wikibuild: no Wine prefix: ${r.error ? r.error.message : (r.stderr || '').trim()}`);
  if (r.stderr) process.stderr.write(r.stderr);
  wineEnvOnce = { ...process.env, WINEPREFIX: r.stdout.trim(), WINEDEBUG: process.env.WINEDEBUG ?? '-all' };
  return wineEnvOnce;
}

// run as a program (by any path to this file, a link included), not imported
if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(new URL(import.meta.url).pathname)) {
  const [a, b, c] = process.argv.slice(2);
  try {
    if (a === '--locked') { if (!fs.existsSync(c)) compile(b, c); }
    else if (TARGETS[a] && !b) console.log(build(a));
    else { console.error('usage: node tools/wiki/wikibuild.mjs host|asan|wine'); process.exit(2); }
  } catch (e) {
    console.error(e.message);
    process.exit(1);
  }
}
