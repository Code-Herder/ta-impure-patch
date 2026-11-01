/* takeover-fixture.mjs -- a takeover entry built for the checks, until art/wiki/entries/takeover.json carries the real
   one: simparity.mjs and framecheck.mjs play it beside the entries. */
import fs from 'node:fs';
import path from 'node:path';

const ROOT = new URL('../../', import.meta.url).pathname;

/* THE TAKEOVER FIXTURE: a takeover entry (`"takeover": true`) on Rewind's field, its four demos the ones
   takeover-spec.md section 9 lays out -- the picker and a seat taken (Bram, seat 1, while Ada's recorded inputs play
   on), the lanes with a kept fork watched and left, a fork saved and two deleted (the watched one after a return to
   the original), and the fourth fork's eviction -- so that the replay layer's Take over is compared in both twins
   demo tick by demo tick. Each click is aimed where its unit stands on its tick in the simulation; a takeover's part
   is quiet through the return glide. */
export function takeoverFixture() {
  const base = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/entries/rewind.json'), 'utf8'));
  // a click selecting what stands at p, and an order to q on the three ticks after, both of `seat`
  const click = (at, seat, p) => [{ at, seat, move: p }, { at, seat, down: 'select' }, { at, seat, up: 'select' }];
  const send = (at, seat, p, q) => [...click(at, seat, p), { at: at + 1, seat, move: q }, { at: at + 2, seat, down: 'order' },
    { at: at + 3, seat, up: 'order' }];
  // the recording: Ada's solar and mex, Bram's commander and walker sent about, Ada's commander moved, b0 killed
  const inputs = [...click(10, 0, [200, 300]), { at: 10, build: 'ARMSOLAR' }, { at: 10, move: [280, 360] }, { at: 10, down: 'lmb' },
    { at: 10, up: 'lmb' }, ...click(80, 0, [140, 330]), { at: 80, build: 'ARMMEX' }, { at: 80, move: [120, 376] },
    { at: 80, down: 'lmb' }, { at: 80, up: 'lmb' }, ...send(200, 1, [520, 120], [460, 200]), ...send(450, 0, [200, 300], [330, 270]),
    ...send(700, 1, [430, 330], [300, 330]), ...send(1200, 1, [460, 200], [560, 160]), { at: 1500, kill: 'b0' }];
  const game = { end: 2700, sidebar: false, inputs };
  const K1 = { id: 1, from: 301, parent: 0, seat: 1, end: 1500, inputs: [...send(340, 1, [460, 200], [300, 120]), ...send(600, 1, [430, 330], [500, 400])] };
  const K2 = { id: 2, from: 601, parent: 0, seat: 0, end: 1800, inputs: send(640, 0, [330, 270], [200, 150]) };
  const K3 = { id: 3, from: 1201, parent: 0, seat: 0, end: 2100, inputs: send(1240, 0, [330, 270], [330, 100]) };
  const at = (t, hud, k = {}) => ({ at: t, hud, ...k });
  const e = { ...base, id: 'takeover-fixture', takeover: true, title: 'Take over a replay', subtitle: 'The replay layer\'s Take over',
    text: ['Take over plays a replay on from any moment, as one of its players.'] };
  delete e.live;
  e.demos = [
    { id: 'takeover', title: 'The picker, a seat chosen', end: 1100, game, inputs: [at(400, 'takeover'), { at: 427, until: 'seeked', max: 120 },
      at(520, 'takeas', { seat: 1 })],
      forks: [{ id: 1, from: 301, parent: 0, seat: 1, inputs: [...send(340, 1, [430, 330], [250, 120]), ...send(420, 1, [460, 200], [600, 300])] }] },
    { id: 'lanes', title: 'The lanes', end: 900, game, kept: [K1], inputs: [at(60, 'lanes'), at(240, 'lane', { fork: 1 }), at(640, 'main'),
      { at: 667, until: 'seeked', max: 120 }] },
    { id: 'save', title: 'A fork saved, two deleted', end: 900, game, kept: [K1, K2], inputs: [at(40, 'play'), at(90, 'lanes'),
      at(160, 'export', { fork: 2 }), at(300, 'delete', { fork: 1 }), at(360, 'delete', { fork: 1 }), at(480, 'lane', { fork: 2 }),
      at(540, 'track', { to: 900 }), { at: 567, until: 'seeked', max: 120 }, at(600, 'delete', { fork: 2 }), at(660, 'delete', { fork: 2 }),
      { at: 687, until: 'seeked', max: 120 }] },
    { id: 'evict', title: 'The fourth fork', end: 1000, game, inputs: [at(40, 'lanes'), at(100, 'lane', { fork: 1 }), at(160, 'main'),
      at(300, 'play'), at(360, 'track', { to: 1100 }), { at: 387, until: 'seeked', max: 120 }, at(420, 'takeover'),
      { at: 447, until: 'seeked', max: 120 }, at(560, 'takeas', { seat: 0 })],
      kept: [{ ...K1, end: 1200 }, { ...K2, seat: 1, end: 1500, inputs: send(640, 1, [430, 330], [500, 400]) }, K3],
      forks: [{ id: 4, from: 901, parent: 0, seat: 0, inputs: send(940, 0, [140, 330], [330, 330]) }] },
  ];
  return e;
}
