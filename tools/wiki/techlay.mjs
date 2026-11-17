/* techlay.mjs -- techtree.js's model and layout of every side of a tech tree pack, printed as
   `wikiassettest techlay` prints tagpu_wikitech.c's: tools/wiki/assetcheck.py --side pairs the two.

   usage: node tools/wiki/techlay.mjs TREE.JSON */
import fs from 'node:fs';
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const WikiTech = require('./mockup/techtree.js');
const tree = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const out = tree.sides.map((_, si) => {
  const M = WikiTech.model(tree, si), L = WikiTech.layout(M), units = {};
  for (const k of M.order) {
    const u = M.units[k];
    units[k] = { kind: u.kind, tier: u.tier, upgrade: +u.upgrade, upgradable: +u.upgradable, first: u.first,
      builds: u.builds, upgrades: u.upgrades, parents: u.parents, unlocks: u.unlocks };
  }
  return { units, spine: M.spine, sp: M.sp, pos: L.pos, w: L.w, h: L.h };
});
process.stdout.write(JSON.stringify(out) + '\n');
