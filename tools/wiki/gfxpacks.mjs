/* gfxpacks.mjs -- what the gfx packs tools/wiki/gfx.py writes (tools/wiki/mockup/gfx/, gitignored: read from the game)
   say of the units' scripts and sounds, read in node for the checks that need them beside the simulation: the C
   test programs' cob.json (tests/wikicobfile.h cob_load; simparity.mjs's replay runs, stagecheck.mjs's stage) and a
   check's own expectation of a death's WAV (simcheck.mjs). The simulation host reads the packs itself
   (tests/wikicobfile.h cob_packs), by the same rule: each type's from the first pack whose scene names it. */
import fs from 'node:fs';
import path from 'node:path';

const ROOT = new URL('../../', import.meta.url).pathname;

/* every type a script of `packs` (gfx pack folders) is for, the first pack's where two name one: name -> {pack (repository-
   relative), cob (the script's absolute path), visible (its posed model's visible flags), offset, parent and top (its
   model's shape, gfx.py `model_shape`; empty and 0 in a pack written before the shape was)} */
export function scripts(packs) {
  const has = {};
  for (const pack of packs) {
    const dir = path.resolve(ROOT, pack), file = path.join(dir, 'scene.json');
    if (!fs.existsSync(file)) continue;
    const meshes = (JSON.parse(fs.readFileSync(file, 'utf8')).units || {}).meshes || {};
    for (const [name, m] of Object.entries(meshes))
      if (m.script && !has[name])
        has[name] = { pack: path.relative(ROOT, dir), cob: path.join(dir, m.script.file), visible: m.script.visible,
                      offset: m.script.offset || [], parent: m.script.parent || [], top: m.script.top || 0 };
  }
  return has;
}

/* every type a sound of `packs` is given for, the first pack's whose scene has its mesh: name -> {cat, count, death},
   the type's sounds.tdf category, its variant count per kind and its death sound (gfx.py `unit_sound`);
   `soundSource(packs)` is that as sound.js's death rule takes a source, which answers null for a type with none */
export function sounds(packs) {
  const has = {};
  for (const pack of packs) {
    const file = path.join(path.resolve(ROOT, pack), 'scene.json');
    if (!fs.existsSync(file)) continue;
    const meshes = (JSON.parse(fs.readFileSync(file, 'utf8')).units || {}).meshes || {};
    for (const [name, m] of Object.entries(meshes)) if (m.sound && !has[name]) has[name] = m.sound;
  }
  return has;
}
export const soundSource = packs => { const has = sounds(packs); return { type: name => has[name] || null }; };
