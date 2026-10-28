/* textparity.mjs -- the wiki text's parity check: the DLL's fill, tagpu/ddraw/src/tagpu_wikitext.c over
   tagpu_wikidata.c's reading of the archive the DLL embeds, against the mockup's tools/wiki/mockup/text.js, byte for
   byte.

   usage: node tools/wiki/textparity.mjs [--wine]
     exit 0 when every fill agrees, 1 when one does not, 2 when the C side cannot be built or run

   It packs art/wiki/ as the Makefile does (tools/hudpak.py --set wiki), builds tagpu/ddraw/tests/wikitexttest.c with
   the real sources, and fills through both every text each entry the tree names shows -- its title, subtitle and
   paragraphs, each demo's title and every caption its inputs carry -- and a table of edge strings (EDGES, below:
   the token rule's corners, unknown names, characters of two to four bytes) with every casting: none, each of
   tools/wiki/casting-fixture.json's, each of every pack present under tools/wiki/mockup/gfx/side/ (gitignored,
   built by tools/wiki/sidepacks.py), one made here whose display names are empty, wide or past the Basic
   Multilingual Plane and one whose every name is WIKI_TITLE_MAX - 1 bytes; each at the interface values 0, 1, 2 and
   -1. Every caption's fill in every one of those must fit the record's caption (text.js CAPTION_MAX - 1 bytes) and
   stay within the bound text.js's captionWorst gave it, which the loaders hold every caption to. It then fills each at interface 1 into the
   buffer sizes that test the cut -- 0 to 3 bytes, the fill's length - 3 to its length + 1, and every size whose cut
   falls inside a character -- the C side by tagpu_wikitext_fill's bound, text.js's fill cut by the same rule here
   (cutBytes). Every fill must be made on both sides, once, with the same bytes and the same cut flag.

   By default the C side is built natively with gcc, AddressSanitizer and UndefinedBehaviorSanitizer. --wine builds it
   with the DLL's own compiler (i686-w64-mingw32-gcc, tagpu_wikidata.c on SSE2 as the Makefile compiles it) and runs it
   under Wine (WINEPREFIX: the main checkout's wineprefix/). */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const WikiText = require('./mockup/text.js');
const ROOT = new URL('../../', import.meta.url).pathname;
const DDRAW = path.join(ROOT, 'tagpu/ddraw');
const WINE = process.argv.includes('--wine');
const SRCS = ['src/tagpu_wikitext.c', 'src/tagpu_wikidata.c', 'src/tagpu_json.c', 'src/tagpu_pak.c', 'src/tagpu_placegeo.c'];
const IFACES = [0, 1, 2, -1];
const WHOLE = 65536;            // wikitexttest.c's WHOLE: a buffer that holds any fill whole

/* the token rule's corners: a "{" with no "}", a lone "}", an empty token, an unknown or capitalised name, a token
   inside braces, spaces in a name, characters of two, three and four bytes beside and inside tokens, every cast
   (some of which no entry's unit and no casting has, so they are copied as written) and every button */
const EDGES = [
  '', 'plain text', '{', '}', '{}', '{order', 'order}', 'a{order}b{Order}c', '{a{order}}', '{{cancel}}', 'x}{y',
  '{Builder}', '{ORDER}', '{ order}', '{order }', '{nope} then {select}', '{Select}{Cancel}{cancel}{select}',
  `{${WikiText.CASTS.join('}, {')}}`, 'trailing {', '{builder}{', 'é{builder}ü',
  '€{mex}\u{1F600}{moho}™', '{\u{1F600}}', '\u{1F527}\u{1F527}{order}\u{1F527}',
];

class CannotRun extends Error {}

function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { maxBuffer: 1 << 30, encoding: 'utf8', ...opts });
  if (r.error || r.status !== 0)
    throw new CannotRun(`${cmd} ${args.join(' ')} failed${r.error ? `: ${r.error.message}` : ` (${r.status})`}\n${r.stderr || ''}`);
  return r.stdout;
}

function build(tmp) {
  if (WINE) {
    const exe = path.join(DDRAW, 'tests/wikitexttest.exe'), sse = 'src/tagpu_wikidata.c';
    const cc = 'i686-w64-mingw32-gcc', flags = ['-Iinc', '-Isrc', '-O2', '-Wall', '-std=c99'];
    const obj = path.join(tmp, 'tagpu_wikidata.o');
    run(cc, [...flags, '-msse2', '-mfpmath=sse', '-mincoming-stack-boundary=2', '-c', '-o', obj, sse], { cwd: DDRAW });
    run(cc, [...flags, '-static', '-o', exe, 'tests/wikitexttest.c', obj, ...SRCS.filter(src => src !== sse)], { cwd: DDRAW });
    const common = run('git', ['rev-parse', '--git-common-dir'], { cwd: ROOT }).trim();
    const env = { ...process.env };
    env.WINEPREFIX ??= path.join(path.resolve(ROOT, common), '..', 'wineprefix');
    env.WINEDEBUG ??= '-all';
    return (args) => run('wine', [exe, ...args], { cwd: tmp, env });
  }
  const exe = path.join(DDRAW, 'tests/wikitexttest-native.exe');   // `.exe` so the tree's *.exe ignore covers it
  run('gcc', ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Isrc', '-O1', '-g', '-fsanitize=address,undefined',
    '-fno-sanitize-recover=all', '-o', exe, 'tests/wikitexttest.c', ...SRCS, '-lm'], { cwd: DDRAW });
  return (args) => run(exe, args, { cwd: tmp });
}

/* tagpu_wikitext_fill's bound on text.js's fill: the bytes of `s` that fit `n` bytes with a terminator, cut at the
   last UTF-8 character ending within them; n 0 keeps nothing and is a cut */
function cutBytes(s, n) {
  const b = Buffer.from(s, 'utf8');
  if (n === 0) return { b: Buffer.alloc(0), cut: 1 };
  if (b.length <= n - 1) return { b, cut: 0 };
  let len = n - 1;
  while (len > 0 && (b[len] & 0xc0) === 0x80) len--;
  return { b: b.subarray(0, len), cut: 1 };
}

/* every text an entry shows, with where it stands, in wikitexttest.c's words; then the edge strings */
function texts(e) {
  const out = [['title', e.title], ['subtitle', e.subtitle]];
  (Array.isArray(e.text) ? e.text : []).forEach((p, i) => out.push([`text[${i}]`, p]));
  (Array.isArray(e.demos) ? e.demos : []).forEach((d, i) => {
    out.push([`demos[${i}].title`, d && d.title]);
    (d && Array.isArray(d.inputs) ? d.inputs : []).forEach((ev, j) => {
      if (ev && 'caption' in ev) out.push([`demos[${i}].inputs[${j}].caption`, ev.caption]);
    });
  });
  EDGES.forEach((s, i) => out.push([`edge[${i}]`, s]));
  return out;
}

/* the castings, each with its name: none, the fixture's, every pack's, and one whose display names test the fill's
   bytes -- an empty title (the type stands in), a cast left unbound (the entry's unit stands in), names of two, three
   and four bytes a character and one of WIKI_TITLE_MAX - 1 bytes */
function castings() {
  const list = [{ name: 'none', v: null }];
  const fixture = JSON.parse(fs.readFileSync(path.join(ROOT, 'tools/wiki/casting-fixture.json'), 'utf8')).castings;
  for (const [name, sides] of Object.entries(fixture)) sides.forEach((C, i) => list.push({ name: `casting-fixture ${name}[${i}]`, v: C }));
  const packs = path.join(ROOT, 'tools/wiki/mockup/gfx/side');
  for (const mod of fs.existsSync(packs) ? fs.readdirSync(packs).sort() : []) {
    const file = path.join(packs, mod, 'side.json');
    if (!fs.existsSync(file)) continue;
    const P = JSON.parse(fs.readFileSync(file, 'utf8'));
    (Array.isArray(P.casting) ? P.casting : []).forEach((C, i) => list.push({ name: `pack ${mod} casting[${i}] ${C && C.side}`, v: C }));
  }
  const wide = JSON.parse(JSON.stringify(fixture.identity[0]));
  Object.assign(wide.casts.builder, { title: 'Bâtisseur' });
  Object.assign(wide.casts.advbuilder, { title: '' });
  Object.assign(wide.casts.mex, { title: 'Extracteur de métal €' });
  Object.assign(wide.casts.moho, { title: 'Mine \u{1F527}' });
  Object.assign(wide.casts.geo, { title: '™'.repeat(31) });
  wide.casts.walker = null;
  list.push({ name: 'wide names', v: wide });
  // the caption's worst case: every cast bound to a name of WIKI_TITLE_MAX - 1 bytes, 31 characters of three
  const worst = JSON.parse(JSON.stringify(fixture.identity[0]));
  for (const k of Object.values(worst.casts)) k.title = '\u2014'.repeat(31);
  list.push({ name: 'worst names', v: worst });
  return list;
}

const show = b => { const s = JSON.stringify(b.toString('utf8')); return s.length > 120 ? `${s.slice(0, 117)}...` : s; };

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'textparity-'));
let failed = 0, fills = 0, cuts = 0, refused = 0, captions = 0;
try {
  const wikitexttest = build(tmp);
  run('python3', [path.join(ROOT, 'tools/hudpak.py'), '--set', 'wiki', path.join(ROOT, 'art/wiki'), path.join(tmp, 'wiki.bin')]);
  const list = castings();
  fs.writeFileSync(path.join(tmp, 'castings.json'), JSON.stringify(list.map(c => c.v)));
  fs.writeFileSync(path.join(tmp, 'edges.json'), JSON.stringify(EDGES));

  const tree = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/tree.json'), 'utf8'));
  const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
  const ids = [...new Set(tree.tree.flatMap(leaves))];
  const got = wikitexttest(['wiki.bin', 'castings.json', 'edges.json']).split(/\r?\n/).filter(l => l.length).map(l => JSON.parse(l));
  const refusedBy = new Set(got.filter(l => 'refused' in l).map(l => l.refused));
  for (const l of got.filter(l => 'refused' in l)) {
    failed++; refused++;
    console.log(`DIFFER casting ${list[l.refused].name}: the C reader refuses it (${l.why}), text.js fills with it`);
  }

  /* what text.js gives, by the line's key: every fill at every interface into the whole buffer, then the cut's sizes;
     and every caption's fill against the caption's bound */
  const want = new Map(), key = l => `${l.e}|${l.c}|${l.i}|${l.n}|${l.w}`;
  for (const id of ids) {
    const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8'));
    list.forEach((C, c) => {
      if (refusedBy.has(c)) return;
      for (const i of IFACES) {
        const w = WikiText.words(entry, C.v, i);
        for (const [where, raw] of texts(entry))
          want.set(key({ e: id, c, i, n: WHOLE, w: where }), { ...cutBytes(WikiText.fillText(raw, w), WHOLE), name: C.name, cutCase: false });
        for (const [where, raw] of texts(entry).filter(([where]) => where.endsWith('.caption'))) {
          const n = Buffer.byteLength(WikiText.fillText(raw, w), 'utf8'), bound = WikiText.captionWorst(raw, entry);
          captions++;
          if (n >= WikiText.CAPTION_MAX || n > bound) {
            failed++;
            console.log(`DOES NOT FIT ${id} ${where} (${C.name}, interface ${i}): ${n} bytes, its bound ${bound}, the caption's ${WikiText.CAPTION_MAX - 1}`);
          }
        }
      }
      const w = WikiText.words(entry, C.v, 1);
      for (const [where, raw] of texts(entry)) {
        const whole = Buffer.from(WikiText.fillText(raw, w), 'utf8');
        for (let n = 0; n <= whole.length + 1; n++) {
          const inChar = n >= 1 && n - 1 < whole.length && (whole[n - 1] & 0xc0) === 0x80;
          if (n <= 3 || n + 3 >= whole.length || inChar)
            want.set(key({ e: id, c, i: 1, n, w: where }), { ...cutBytes(whole.toString('utf8'), n), name: C.name, cutCase: true });
        }
      }
    });
  }

  const seen = new Set();
  for (const l of got.filter(l => !('refused' in l))) {
    const k = key(l), j = want.get(k), c = Buffer.from(l.t, 'hex');
    const at = `${l.e} ${l.w} (${j ? j.name : `casting ${l.c}`}, interface ${l.i}${l.n === WHOLE ? '' : `, ${l.n} bytes`})`;
    if (!j) { failed++; console.log(`DIFFER ${at}: C fills it, text.js has no such text`); continue; }
    if (seen.has(k)) { failed++; console.log(`DIFFER ${at}: C fills it twice`); continue; }
    seen.add(k);
    if (j.cutCase) cuts++; else fills++;
    if (!c.equals(j.b) || l.cut !== j.cut) {
      failed++;
      console.log(`DIFFER ${at}: C ${show(c)}${l.cut ? ' (cut)' : ''}, text.js ${show(j.b)}${j.cut ? ' (cut)' : ''}`);
    }
  }
  for (const [k, j] of want)
    if (!seen.has(k)) { failed++; console.log(`DIFFER ${k.split('|').join(' ')} (${j.name}): text.js fills it, C does not`); }
  console.log(`${list.length} castings x ${IFACES.length} interface values over ${ids.length} entries and ${EDGES.length} edge strings`);
} catch (e) {
  if (!(e instanceof CannotRun)) throw e;
  console.error(`textparity: ${e.message}`);
  process.exitCode = 2;
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
if (process.exitCode !== 2) {
  const variant = WINE ? 'i686 under Wine' : 'native';
  console.log(failed ? `${failed} differ (of ${fills} fills and ${cuts} cut fills compared${refused ? `, ${refused} castings refused by C` : ''}; C fill: ${variant})`
    : `0 differ: all ${fills} fills and ${cuts} cut fills equal, byte for byte, and all ${captions} caption fills fit (C fill: ${variant})`);
  process.exitCode = failed ? 1 : 0;
}
