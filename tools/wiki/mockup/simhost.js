/* simhost.js -- the in-game wiki's simulation for the mockup and the node checks: the DLL's own C core
   (tagpu/ddraw/src/tagpu_wikisim.c over tasim_*.c, the loader tagpu_wikidata.c, the COB host tagpu_wikicob.c) run as
   a host process (tagpu/ddraw/tests/wikihost.c, `wikitest serve`, which states the protocol) and answered through
   this module's calls. research/notes/ingame-wiki.md, "The simulation's contract", is its specification; the C is its
   implementation, and there is no other.

   In a page the request is a synchronous POST to /sim (tools/wiki/serve.py relays it to its host); under node it is
   tools/wiki/hostchan.cjs's channel to a `host` build, or the tee of the builds WIKI_HOST_TEE names (host,asan,wine),
   which answers with the first and records where any other differs. The global WikiSim and module.exports.

   THE OBJECTS SENT ONCE. An entry, a layout, a casting and a sight table are sent the first time they are used and
   named by a handle after that: a WeakMap from the object to its handle, then a Map from its JSON text, so a re-made
   replay's game and a frame's per-tick camera send nothing. An object passed here is treated as unchanged afterwards;
   SIMHOST_NOCACHE=1 (node) keys by the text alone, which a check uses to show the difference is none. The host keeps
   a least-recently-used table of each kind, so a handle it no longer holds is sent again.

   THE ECHO. The queries made between a sim's last two changes -- its views by seat, every unit's orders, its hash,
   its gameNow by seat, its poses -- ride on the next change's answer (`want`), and until the change after it they
   are answered from there. A page that steps and then draws makes one request a tick, and a check reading view,
   orders and hash every tick the same. A query outside the echo is a request of its own.

   THE QUEUE. input and release answer nothing a caller reads, so they wait for the next request and go with it in
   one batch; an input the host refuses throws at that next call, the one place this differs from a call that throws
   where it is made. A sim the caller drops is freed the same way once the garbage collector has seen it go. */
const WikiSim = (() => {
  const NODE = typeof window === 'undefined';
  const PoseRef = () => typeof WikiPose !== 'undefined' ? WikiPose : require('./pose.js');
  let chan = null;
  function channel() {
    if (chan) return chan;
    if (NODE) {
      const hc = require('../hostchan.cjs'), tee = process.env.WIKI_HOST_TEE;
      chan = tee ? hc.tee(tee.split(',')) : hc.open('host');
    } else {
      chan = {
        request(line) {
          const x = new XMLHttpRequest();
          x.open('POST', '/sim', false);
          x.setRequestHeader('Content-Type', 'application/json');
          x.send(line);
          if (x.status !== 200) throw new Error(`wiki sim: /sim answered ${x.status}: the wiki's simulation runs in tools/wiki/serve.py`);
          return x.responseText;
        },
      };
    }
    return chan;
  }

  // the calls waiting for the next request, each with what takes its answer
  let queue = [];
  const freed = typeof FinalizationRegistry !== 'undefined' ? new FinalizationRegistry(h => queue.push({ req: { op: 'free', sim: h }, take: () => {} })) : null;
  /* one request, answered; the queue goes first in one batch, and the first queued call the host refused throws */
  function ask(req) {
    let a;
    if (queue.length) {
      const q = queue;
      queue = [];
      const all = JSON.parse(channel().request(JSON.stringify({ op: 'batch', calls: [...q.map(c => c.req), req] }))).answers;
      let err = null;
      q.forEach((c, i) => { if (all[i] && all[i].error) err ??= all[i].error; else c.take(all[i]); });
      a = all[all.length - 1];
      if (err !== null) throw new Error(err);
    } else a = JSON.parse(channel().request(JSON.stringify(req)));
    if (a && a.error) throw new Error(a.error);
    return a;
  }

  /* THE OBJECTS SENT ONCE: kind's handle for `obj`, sent when none is known; `json` what is sent for it */
  const NOCACHE = NODE && typeof process !== 'undefined' && process.env.SIMHOST_NOCACHE === '1';
  const held = { entry: [new WeakMap(), new Map()], layout: [new WeakMap(), new Map()], casting: [new WeakMap(), new Map()],
    table: [new WeakMap(), new Map()] };
  function handle(kind, obj, json = obj) {
    const [byObj, byText] = held[kind];
    let h = !NOCACHE && byObj.get(obj);
    if (h) return h;
    const text = JSON.stringify(json);
    h = byText.get(text);
    if (!h) { h = ask({ op: kind, json })[kind]; byText.set(text, h); }
    if (!NOCACHE) byObj.set(obj, h);
    return h;
  }
  function forget(kind, h) {
    const [, byText] = held[kind];
    for (const [k, v] of byText) if (v === h) byText.delete(k);
    held[kind][0] = new WeakMap();
  }
  /* `f` with handles that may be gone from the host's tables: a "no <kind> N" refusal forgets N and runs `f` again */
  function withHandles(f) {
    for (let n = 0; ; n++) {
      try { return f(); } catch (e) {
        const m = /^no (entry|layout|casting|table) (\d+)$/.exec(e.message);
        if (!m || n > 8) throw e;
        forget(m[1], +m[2]);
      }
    }
  }
  // an entry as a sim is made from one of its demos: that demo its only one, as the host's `new` reads it
  const asDemo = new WeakMap(), NO_DEMO = {};
  function entryOf(entry, demo) {
    let m = asDemo.get(entry);
    if (!m) asDemo.set(entry, m = new WeakMap());
    const key = demo || NO_DEMO;
    let e = m.get(key);
    if (!e) m.set(key, e = { ...entry, demos: demo ? [demo] : [] });
    return handle('entry', e);
  }
  // the packs' handle for a list of gfx pack folders, relative to the repository's root
  const packsOf = new Map();
  function packs(dirs) {
    const k = JSON.stringify(dirs);
    if (!packsOf.has(k)) packsOf.set(k, ask({ op: 'packs', dirs }).packs);
    return packsOf.get(k);
  }

  let K = null;
  const hello = () => K ??= ask({ op: 'hello' }).constants;

  const b64 = s => NODE ? new Uint8Array(Buffer.from(s, 'base64')) : Uint8Array.from(atob(s), c => c.charCodeAt(0));
  const copy = v => v === undefined ? v : JSON.parse(JSON.stringify(v));

  /* A SIM made by `new`: play 'try', 'demo', 'game' or 'stall'; ctx {side, casting, iface, rival, packs, scripts, sounds},
     `packs` the gfx pack folders its units' scripts and sounds are read from (none: neither), `scripts` or `sounds`
     false to leave that half out */
  function make(entry, demo, play, layout, ctx) {
    const c = ctx || null;
    const made = withHandles(() => ask({ op: 'new', entry: entryOf(entry, demo), play, layout: layout == null ? null : handle('layout', layout),
      ctx: c && { side: c.side, iface: c.iface, casting: c.casting ? handle('casting', c.casting) : null,
        rival: c.rival ? handle('casting', c.rival) : null, packs: c.packs && c.packs.length ? packs(c.packs) : null,
        scripts: c.scripts !== false, sounds: c.sounds !== false } }));
    const h = made.sim, map = Object.freeze(made.map);
    let asked = { views: new Set(), orders: false, hash: false, now: new Set(), poses: false }, echo = null;
    const sim = { tick: made.tick, done: made.done, side: made.side, iface: made.iface, game: made.game, types: made.types,
      nin: made.nin, log: [...made.log] };
    const fresh = () => { asked = { views: new Set(), orders: false, hash: false, now: new Set(), poses: false }; };
    const want = () => ({ ...(asked.views.size ? { view: [...asked.views] } : {}), ...(asked.orders ? { orders: true } : {}),
      ...(asked.hash ? { hash: true } : {}), ...(asked.now.size && sim.game ? { now: [...asked.now] } : {}), ...(asked.poses ? { poses: true } : {}) });
    // a change's answer: the sim's tick, done and log, and the echo of what was asked between the last two changes
    const took = a => {
      sim.tick = a.tick;
      sim.done = a.done;
      for (const e of a.log || []) sim.log.push(e);
      echo = { views: a.views || {}, orders: a.orders, hash: a.hash, now: a.now || {}, poses: a.poses };
      fresh();
      return a;
    };
    const change = req => took(ask({ ...req, sim: h, want: want() }));
    const queued = req => { echo = null; queue.push({ req: { ...req, sim: h }, take: a => { sim.tick = a.tick; sim.done = a.done; for (const e of a.log || []) sim.log.push(e); } }); };
    const viewOf = v => { v.map = map; return v; };
    Object.assign(sim, {
      input(ev) { queued({ op: 'input', ev }); },
      release(seat = 0) { queued({ op: 'release', seat }); },
      step() { change({ op: 'step', n: 1 }); },
      // up to n ticks in one request (fewer once done): each tick's `each` values ("hash", "now" of `seat`)
      steps(n, each = [], seat = 0) { return change({ op: 'step', n, each, seat }).each; },
      fork(R, inputs, mode, seat = 0) { change({ op: 'fork', R, inputs, mode, seat }); },
      view(seat = 0) {
        asked.views.add(seat);
        const e = echo && echo.views[seat];
        return viewOf(e ? copy(e) : ask({ op: 'view', sim: h, seat }));
      },
      orders(id) {
        asked.orders = true;
        if (echo && echo.orders) return copy(echo.orders[id] ?? null);
        return ask({ op: 'orders', sim: h, id });
      },
      hash() {
        asked.hash = true;
        return echo && echo.hash ? echo.hash : ask({ op: 'hash', sim: h }).hash;
      },
      poses() {
        asked.poses = true;
        const p = echo && echo.poses ? echo.poses : ask({ op: 'poses', sim: h });
        return p.map(s => s ? PoseRef().parse(b64(s)) : null);
      },
      cobNotes() { return ask({ op: 'notes', sim: h }); },
      free() { freed && freed.unregister(sim); queue.push({ req: { op: 'free', sim: h }, take: () => {} }); },
    });
    if (sim.game) sim.gameNow = (seat = 0) => {
      asked.now.add(seat);
      const e = echo && echo.now[seat];
      return e ? copy(e) : ask({ op: 'now', sim: h, seat });
    };
    if (freed) freed.register(sim, h, sim);
    return sim;
  }

  /* THE SIGHT over a game's view (tagpu_wikisim_sight_lit, tagpu_wikisim_contacts): the view's units, deaths and types
     as the host reads them, its types by index; each lit grid's contacts kept beside it for sightContacts */
  const contactsOf = new WeakMap();
  function sightLit(v, S, seat, g) {
    const types = v.game ? v.game.types : [], T = new Map(types.map((t, i) => [t.name, i]));
    const ix = n => T.has(n) ? T.get(n) : -1;
    const a = withHandles(() => ask({ op: 'sight', table: S ? handle('table', S) : null, seat, tick: g, contacts: true, view: {
      w: v.map.w, h: v.map.h, units: v.units.map(u => [u.x, u.z, u.owner, ix(u.type), u.built]),
      deaths: v.game ? v.game.deaths.map(d => [d.tick, d.owner, d.x, d.z, ix(d.type)]) : [],
      types: types.map(t => [t.sightDistance, t.radarDistance, t.sonarDistance, t.activates ? 1 : 0, t.modelHeight]) } }));
    const lit = b64(a.lit);
    contactsOf.set(lit, { v, seat, contacts: a.contacts });
    return lit;
  }
  function sightContacts(v, lit, seat) {
    const c = contactsOf.get(lit);
    if (!c || c.v !== v || c.seat !== seat) throw new Error('wiki sim: sightContacts takes the grid sightLit made of the same view and seat');
    return c.contacts.slice();
  }
  const sighttable = (S, d) => S ? withHandles(() => ask({ op: 'sighttable', table: handle('table', S), distance: d }))
    : ask({ op: 'sighttable', table: null, distance: d });

  const api = {
    create: (entry, demo, layout, ctx) => make(entry, demo, 'try', layout, ctx),
    play: (entry, demo, layout, ctx) => make(entry, demo, 'demo', layout, ctx),
    // a game entry's demo's game script, its economy in `mode` 'refuse' (an authored script) or 'stall' (a seat the
    // player plays)
    game: (entry, demo, layout, ctx, mode = 'refuse') => make(entry, demo, mode === 'stall' ? 'stall' : 'game', layout, ctx),
    // a layout's and a casting's refusal, thrown with the loader's words; the object back when it is read
    checkLayout(L) { handle('layout', L); return L; },
    checkCasting(C) { handle('casting', C); return C; },
    // a text entry's refusal (null: none), as tagpu_wikidata_entry's
    checkText: entry => ask({ op: 'text', json: entry }).why,
    // the scene a demo plays on (null: the first)
    sceneOf: (entry, demo) => entry.scenes[withHandles(() => ask({ op: 'scene', entry: entryOf(entry, demo) })).scene],
    // a players entry's seat's camera at a tick (tagpu_wikidata_seat_cam), null for a seat with none
    seatCam: (entry, demo, k, t) => withHandles(() => ask({ op: 'seatcam', entry: entryOf(entry, demo), seat: k, tick: t })).at,
    // the game frame's 4:3 bound on a game entry's map (tagpu_wikidata_frame_fits)
    frameFits: (w, h) => ask({ op: 'fits', w, h }).fits,
    sightLit, sightContacts,
    // a layout's sight table's index for a sight distance, and its table (null: none)
    sightIndex: (S, d) => sighttable(S, d).idx,
    sightTable: (S, d) => sighttable(S, d).table,
  };
  // the constants, tagpu_wikisim.h's and tagpu_wikidata.h's tables as the host's hello names them, read at first use
  for (const k of ['TRIP', 'TRIP_DOWN', 'TRIP_UP', 'TRIP_ROWS', 'PICK', 'PICK_DOWN', 'PICK_UP', 'KINDS', 'KEYS', 'SND_NAMES', 'UNTILS',
    'CASTS', 'HUD_CONTROLS', 'REPLAY_CONTROLS', 'LIVE_CONTROLS', 'TAKEOVER_CONTROLS', 'PLAYERS_CONTROLS', 'CHIPS', 'SND_DEATH',
    'SEATS', 'GAME_KILLS', 'GAME_END_MAX', 'FEATS_MAX', 'HUD_DOWN', 'HUD_UP', 'PLAYERS_DEATHS', 'PLAYERS_DEATH_SPAN', 'SIGHT_MIN',
    'SIGHT_NUMTABLES', 'SIGHT_IDX', 'SIGHT_REACH', 'SIGHT_CELLS', 'SIGHT_SPAN', 'DEMO_FORKS', 'TIMELINES'])
    Object.defineProperty(api, k, { enumerable: true, get: () => hello()[k] });
  return api;
})();
if (typeof module !== 'undefined') module.exports = WikiSim;
