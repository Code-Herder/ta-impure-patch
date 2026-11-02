/* players-fixture.mjs -- a players entry built for the checks beside the tree's art/wiki/entries/players.json, its
   figures invented and its scratch entry's Bram with no camera: simparity.mjs plays both beside the entries and edits
   the fixture for its refusal cases. */
import fs from 'node:fs';
import path from 'node:path';

const ROOT = new URL('../../', import.meta.url).pathname;

/* THE PLAYERS FIXTURE: Watching's entry made a players entry (`"players": true`) -- a screen of 56 x 30 cells, the
   narrowest that makes a 4:3 frame; the map grown to 64 x 44, even and as deep as the viewport beside the deepest
   column; each seat a camera and a team; each type a sight whose table the fixture layout stores (art/wiki/side/fixture.json,
   the stock indices 3, 6, 7 and 8) -- and its game given camera inputs and
   killers, so that both twins are shown to read them alike and to leave them out of the simulation. Every figure is
   invented for the checks: none is read from a game file. Ada's moves lie at x 120..298, z 252..376 from tick 12 to
   92, inside every view her camera holds. */
export function playersFixture() {
  const base = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/entries/watching.json'), 'utf8'));
  const e = { ...base, id: 'players-fixture', players: true, screen: [56, 30], title: 'Players and the camera',
    subtitle: 'The replay layer\'s players', text: ['A players replay follows each seat\'s camera.'] };
  const SIGHT = { ARMCOM: 300, ARMCK: 260, ARMMEX: 100, ARMSOLAR: 200, CORCOM: 300, CORAK: 240 };
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

/* THE PLAYERS SCRATCH ENTRY: the fixture with Bram's camera not recorded (his scene camera and camera input gone), so
   Jump and Follow on his row toast, and one demo whose viewer presses the Players set -- Jump and Follow on Ada, the
   switches, the stats window and its chips, a seek to before w1's death and Sight on Bram through it (his death vision
   and Ada's units as his commander's radar contacts), the pause, Sight changes while paused (the bar's re-read), a
   seek back while paused (every DisplayTimer to the shown tick), Follow off, Jump and Follow on Bram, close and a
   switch -- so that both twins are shown to play the replay layer's players state alike. */
export function playersScratch() {
  const e = playersFixture();
  e.id = 'players-scratch';
  e.scenes = Object.fromEntries(Object.entries(e.scenes).map(([n, sc]) => [n, { ...sc,
    seats: sc.seats.map((z, k) => { if (k !== 1) return z; const { camera, ...rest } = z; return rest; }) }]));
  const base = e.demos[0];
  const inputs = [{ at: 0, caption: 'The Players set.' }, { at: 10, hud: 'jump', seat: 0 }, { at: 40, hud: 'follow', seat: 0 },
    { at: 70, hud: 'cursors' }, { at: 100, hud: 'stats' }, { at: 130, hud: 'chip', chip: 'army' }, { at: 160, hud: 'chip', chip: 'team' },
    { at: 190, hud: 'track', to: 1150 }, { at: 220, until: 'seeked', max: 120 }, { at: 230, hud: 'sight', seat: 1 },
    { at: 290, hud: 'play' }, { at: 320, hud: 'sight', seat: 0 }, { at: 350, hud: 'sight', seat: 0 }, { at: 380, hud: 'sight', seat: 1 },
    { at: 410, hud: 'rew' }, { at: 440, hud: 'play' }, { at: 470, hud: 'follow', seat: 0 }, { at: 500, hud: 'jump', seat: 1 },
    { at: 530, hud: 'follow', seat: 1 }, { at: 560, hud: 'close' }, { at: 590, hud: 'selections' }];
  e.demos = [{ ...base, id: 'players', end: 640, inputs,
    game: { ...base.game, inputs: base.game.inputs.filter(x => !('camera' in x && x.seat === 1)) } }];
  return e;
}
