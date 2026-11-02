/* players-fixture.mjs -- a players entry built for the checks, until art/wiki/entries/players.json carries the real
   one: simparity.mjs plays it beside the entries and edits it for its refusal cases. */
import fs from 'node:fs';
import path from 'node:path';

const ROOT = new URL('../../', import.meta.url).pathname;

/* THE PLAYERS FIXTURE: Watching's entry made a players entry (`"players": true`) -- a screen of 56 x 30 cells, the
   narrowest that makes a 4:3 frame; the map grown to 64 x 44, even and as deep as the viewport beside the deepest
   column; each seat a camera and a team; each type a sight above the floor -- and its game given camera inputs and
   killers, so that both twins are shown to read them alike and to leave them out of the simulation. Every figure is
   invented for the checks: none is read from a game file. Ada's moves lie at x 120..298, z 252..376 from tick 12 to
   92, inside every view her camera holds. */
export function playersFixture() {
  const base = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/entries/watching.json'), 'utf8'));
  const e = { ...base, id: 'players-fixture', players: true, screen: [56, 30], title: 'Players and the camera',
    subtitle: 'The replay layer\'s players', text: ['A players replay follows each seat\'s camera.'] };
  const SIGHT = { ARMCOM: 300, ARMCK: 260, ARMMEX: 160, ARMSOLAR: 160, CORCOM: 300, CORAK: 240 };
  e.units = Object.fromEntries(Object.entries(base.units).map(([t, d]) => [t, { ...d, sightDistance: SIGHT[t] ?? 200,
    modelHeight: 30, ...(d.cast === 'commander' ? { radarDistance: 700, sonarDistance: 0 } : {}) }]));
  const cams = [[448, 352], [576, 240]];
  e.scenes = Object.fromEntries(Object.entries(base.scenes).map(([n, sc]) => [n, { ...sc, map: { ...sc.map, w: 64, h: 44 },
    seats: sc.seats.map((z, k) => ({ ...z, camera: cams[k % cams.length], team: k })) }]));
  const camera = [{ at: 200, seat: 0, camera: [500, 320], over: 60 }, { at: 600, seat: 1, camera: [520, 300] },
    { at: 900, seat: 0, camera: [448, 352], over: 120 }];
  e.demos = base.demos.map(d => ({ ...d, game: { ...d.game, sidebar: false,
    inputs: [...d.game.inputs.map(x => 'kill' in x ? { ...x, by: 0 } : x), ...camera] } }));
  return e;
}
