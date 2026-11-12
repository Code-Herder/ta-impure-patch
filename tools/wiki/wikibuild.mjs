/* wikibuild.mjs -- the one recipe for the wiki's simulation host, tagpu/ddraw/tests/wikitest.c (`wikitest serve`,
   tests/wikihost.c): its sources and its three builds' flags live here and nowhere else.

     host   the mockup's host (tools/wiki/serve.py's /sim, the node checks through hostchan.cjs): gcc -O2. No -march,
            so no FMA, and -std=c99 keeps -ffp-contract=off: the doubles round as the DLL's SSE2 build rounds them.
     asan   the same under AddressSanitizer and UBSan, every finding fatal, a double converted to an integer it
            does not fit included (float-cast-overflow, which -fsanitize=undefined leaves out).
     wine   the DLL's own compiler, i686-w64-mingw32-gcc, at its default -march as `make` runs it, the simulation's
            sources on SSE2 as the Makefile compiles them, run under Wine. __USE_MINGW_ANSI_STDIO makes its %.17g
            print as glibc's does (msvcrt writes three exponent digits), so the three builds' answers compare as text;
            it touches this test program only.

   build(target) returns the executable's path, built once per content: build/wikihost/<digest>/wikitest[.exe], the
   digest a sha256 of what decides the executable -- the target's compiler, flags and sources, the SSE2 sources and
   flags, the compiler's --version, this file, and the bytes of every source and of every header the compiler names
   for them (-MM, the repository's own: the system's go with the compiler's version). A build compiles into a
   temporary of its own and publishes it by rename, under an flock(1) on build/wikihost/<target>.lock, so processes
   started together (the shards, serve.py, the node checks) build each target once and never run a half-written file;
   a builder that dies releases the lock with its process. The builder takes the digest again once it has compiled
   and publishes nothing when it differs: a file edited during the compile could otherwise sit under a digest of
   content it was not built from. CLI: `node tools/wiki/wikibuild.mjs host|asan|wine` prints the path and says on
   stderr when it compiled. */
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { TASIM_SRCS } from './tasimsrcs.mjs';

const ROOT = path.resolve(new URL('../..', import.meta.url).pathname);
const DDRAW = path.join(ROOT, 'tagpu/ddraw');
const CACHE = path.join(ROOT, 'build/wikihost');

/* the simulation (its front end and the core), the loader, the replay layer, the COB host, and the stage with what
   it draws through (tagpu_wikistage_pose for the `poses` op) */
export const SOURCES = ['tests/wikitest.c', 'tests/wikihost.c', 'src/tagpu_wikisim.c', ...TASIM_SRCS, 'src/tagpu_wikidata.c',
  'src/tagpu_wikireplay.c', 'src/tagpu_wikistage.c', 'src/tagpu_wikiframe.c', 'src/tagpu_hudlay.c', 'src/tagpu_json.c',
  'src/tagpu_pak.c', 'src/tagpu_placegeo.c', 'src/tagpu_sweepgeo.c', 'src/tagpu_wikicob.c', 'src/tagpu_cobvm.c',
  'src/tagpu_cob_safe.c'];
/* the sources the Makefile compiles on SSE2 (its CFLAGS += -msse2 lines) */
const SSE = new Set(['src/tagpu_wikisim.c', ...TASIM_SRCS, 'src/tagpu_wikidata.c', 'src/tagpu_wikicob.c', 'src/tagpu_sweepgeo.c',
  'src/tagpu_wikireplay.c', 'src/tagpu_wikistage.c', 'src/tagpu_wikiframe.c']);
const SSE_FLAGS = ['-msse2', '-mfpmath=sse', '-mincoming-stack-boundary=2'];

const TARGETS = {
  host: { cc: 'gcc', flags: ['-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Iinc'], libs: ['-lm'], exe: 'wikitest' },
  asan: { cc: 'gcc', flags: ['-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Iinc', '-fsanitize=address,undefined',
    '-fsanitize=float-cast-overflow', '-fno-sanitize-recover=all'], libs: ['-lm'], exe: 'wikitest' },
  wine: { cc: 'i686-w64-mingw32-gcc', flags: ['-std=c99', '-O2', '-Wall', '-Iinc', '-Isrc', '-D__USE_MINGW_ANSI_STDIO=1'], libs: ['-static'],
    exe: 'wikitest.exe', sse: true },
};

function run(cmd, args) {
  const r = spawnSync(cmd, args, { cwd: DDRAW, encoding: 'utf8', maxBuffer: 1 << 26 });
  if (r.error || r.status !== 0) throw new Error(`${cmd} ${args.join(' ')} failed${r.error ? `: ${r.error.message}` : ''}\n${r.stderr || ''}`);
  return r.stdout;
}

/* every file a build of `name` reads: the sources, and the headers the compiler names for them with the flags each is
   compiled with (-MM: the repository's own headers, which a source may include under one flag and not another) */
function inputs(name) {
  const t = TARGETS[name];
  const groups = t.sse ? [[SOURCES.filter(s => !SSE.has(s)), []], [[...SSE], SSE_FLAGS]] : [[SOURCES, []]];
  const files = new Set(SOURCES);
  for (const [srcs, extra] of groups)
    for (const w of run(t.cc, [...t.flags, ...extra, '-MM', ...srcs]).replace(/\\\n/g, ' ').split(/\s+/))
      if (w && !w.endsWith(':')) files.add(path.normalize(w));
  return [...files].sort();
}

function digest(name) {
  const t = TARGETS[name], h = crypto.createHash('sha256');
  h.update(JSON.stringify([name, t, SOURCES, t.sse ? [[...SSE], SSE_FLAGS] : null, run(t.cc, ['--version'])]));
  h.update(fs.readFileSync(new URL(import.meta.url)));
  for (const f of inputs(name)) h.update(`${f}\0`).update(fs.readFileSync(path.join(DDRAW, f))).update('\0');
  return h.digest('hex');
}

/* compile `name` into `out`, whose folder is named by the digest the caller took, through a temporary of this
   process's own, published only when the digest taken after compiling is still that one; the caller holds the lock */
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
    if (digest(name) !== path.basename(path.dirname(out)))
      throw new Error(`wikibuild: ${name}'s sources changed while it compiled; nothing is kept, build again`);
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
