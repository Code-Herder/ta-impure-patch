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
   a least-recently-used table of each kind, packs sets included, so a handle it no longer holds is sent again.

   THE INSTANCE (tests/wikihost.c). Every call is stamped with the id of the host its handles came from: an object's
   and a pure answer's with the current host's, a sim's with the host that made it. A host refuses a call of another
   id before it reads a handle, so no handle is ever read by a host that did not give it -- serve.py starts a new host
   after one ends, numbering from 1 again. The refusal names the host that answered; when that is a new one, every
   kept handle and answer is dropped and the call made again from scratch, so its objects are sent anew, and a sim
   of the ended host throws at each call from then on.

   THE ECHO. The queries made between a sim's last two changes -- its views by seat, every unit's orders, its hash,
   its gameNow by seat, its poses -- ride on the next change's answer (`want`), and until the change after it they
   are answered from there. A page that steps and then draws makes one request a tick, and a check reading view,
   orders and hash every tick the same. A query outside the echo is a request of its own.

   THE QUEUE. input and release answer nothing a caller reads, so they wait for the next request and go with it in
   one batch; an input the host refuses throws at that next call, the one place this differs from a call that throws
   where it is made. A sim the caller drops is freed the same way once the garbage collector has seen it go, and a
   free's answer is no one's: a free of a sim the host no longer holds is not an error to throw into an unrelated
   call. */
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

  /* THE INSTANCE: `inst` the id of the current host, from its hello, and K its constants; null until asked, and again
     once a refusal has shown that host gone */
  let inst = null, K = null;
  function hello() {
    if (inst === null) {
      const a = JSON.parse(channel().request('{"op":"hello"}'));
      if (a.error) throw new Error(a.error);
      inst = a.instance;
      K = a.constants;
    }
    return inst;
  }
  // a call refused because the host that gave its handles has ended, and a new host now answers
  class HostChanged extends Error {}
  /* a refusal for another host, which names the host that answered: when that is not the current one, every handle
     and answer kept is dropped (they name nothing in the new host); true when it was */
  function refused(a) {
    if (a.host === inst) return false;
    for (const k in held) held[k] = [new WeakMap(), new Map()];
    packsOf.clear();
    pureKept.clear();
    inst = null;
    return true;
  }

  /* the calls waiting for the next request, each with what takes its answer; `quiet` for a free, whose answer no one
     reads */
  let queue = [];
  const freed = typeof FinalizationRegistry !== 'undefined'
    ? new FinalizationRegistry(({ h, at }) => queue.push({ req: { op: 'free', sim: h, in: at }, take: () => {}, quiet: true })) : null;
  /* one request, answered, stamped with the current host's id unless it names its own; the queue goes first in one
     batch, and the first queued call the host refused throws, a free's answer and a refusal for another host aside
     (the call's sim is of an ended host, and its own next call says so) */
  function ask(req) {
    const r = req.in === undefined ? { ...req, in: hello() } : req;
    let a;
    if (queue.length) {
      const q = queue;
      queue = [];
      const got = JSON.parse(channel().request(JSON.stringify({ op: 'batch', calls: [...q.map(c => c.req), r] })));
      if (got.error) throw new Error(got.error);
      const all = got.answers;
      let err = null;
      q.forEach((c, i) => {
        const x = all[i];
        if (x && x.host !== undefined) refused(x);
        else if (x && x.error) { if (!c.quiet) err ??= x.error; }
        else c.take(x);
      });
      a = all[all.length - 1];
      if (err !== null && !(a && a.host !== undefined)) throw new Error(err);
    } else a = JSON.parse(channel().request(JSON.stringify(r)));
    if (a && a.host !== undefined && refused(a)) throw new HostChanged(a.error);
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
    const byText = kind === 'packs' ? packsOf : held[kind][1];
    for (const [k, v] of byText) if (v === h) byText.delete(k);
    if (kind !== 'packs') held[kind][0] = new WeakMap();
  }
  /* `f` with handles that may be gone from the host's tables: a "no <kind> N" refusal forgets N, and a new host's
     refusal has dropped every handle already; either way `f` runs again and sends what it needs anew */
  function withHandles(f) {
    for (let n = 0; ; n++) {
      try { return f(); } catch (e) {
        const m = /^no (entry|layout|casting|table|packs) (\d+)$/.exec(e.message);
        if (!(m || e instanceof HostChanged) || n > 8) throw e;
        if (m) forget(m[1], +m[2]);
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

  /* THE PURE ANSWERS: an op that reads no sim (sight, sighttable, scene, seatcam, fits) answers one request text alike
     -- a handle names one object for its host's life, never reused, and the text carries the host's id -- so the last
     PURE_KEEP answers are kept by their request's text, and a frame drawn again over the same tick asks nothing */
  const PURE_KEEP = 64, pureKept = new Map();
  function pure(req) {
    const r = { ...req, in: hello() }, k = JSON.stringify(r);
    let a = pureKept.get(k);
    if (a === undefined) {
      a = ask(r);
      pureKept.set(k, a);
      if (pureKept.size > PURE_KEEP) pureKept.delete(pureKept.keys().next().value);
    }
    return a;
  }
  const constants = () => { hello(); return K; };

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
    // its host is the one that answered `new`: the current one, since nothing refused the call
    const h = made.sim, at = inst, map = Object.freeze(made.map);
    const own = req => ask({ ...req, sim: h, in: at });
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
    const change = req => took(own({ ...req, want: want() }));
    const queued = req => { echo = null; queue.push({ req: { ...req, sim: h, in: at }, take: a => { sim.tick = a.tick; sim.done = a.done; for (const e of a.log || []) sim.log.push(e); } }); };
    const viewOf = v => { v.map = map; return v; };
    Object.assign(sim, {
      input(ev) { queued({ op: 'input', ev }); },
      release(seat = 0) { queued({ op: 'release', seat }); },
      step() { change({ op: 'step', n: 1 }); },
      // up to n ticks in one request (fewer once done): each tick's `each` values ("hash", "now" of `seat`)
      steps(n, each = [], seat = 0) { return change({ op: 'step', n, each, seat }).each; },
      fork(R, inputs, mode, seat = 0) { change({ op: 'fork', R, inputs, mode, seat }); },
      // `map` true asks the host for the map again rather than the one the sim was made with: a check's, which holds
      // the two equal
      view(seat = 0, map = false) {
        if (map) return own({ op: 'view', seat, map: true });
        asked.views.add(seat);
        const e = echo && echo.views[seat];
        return viewOf(e ? copy(e) : own({ op: 'view', seat }));
      },
      orders(id) {
        asked.orders = true;
        if (echo && echo.orders) return copy(echo.orders[id] ?? null);
        return own({ op: 'orders', id });
      },
      hash() {
        asked.hash = true;
        return echo && echo.hash ? echo.hash : own({ op: 'hash' }).hash;
      },
      poses() {
        asked.poses = true;
        const p = echo && echo.poses ? echo.poses : own({ op: 'poses' });
        return p.map(s => s ? PoseRef().parse(b64(s)) : null);
      },
      cobNotes() { return own({ op: 'notes' }); },
      free() { freed && freed.unregister(sim); queue.push({ req: { op: 'free', sim: h, in: at }, take: () => {}, quiet: true }); },
    });
    if (sim.game) sim.gameNow = (seat = 0) => {
      asked.now.add(seat);
      const e = echo && echo.now[seat];
      return e ? copy(e) : own({ op: 'now', seat });
    };
    if (freed) freed.register(sim, { h, at }, sim);
    return sim;
  }

  /* THE SIGHT over a game's view (tagpu_wikisim_sight_lit, tagpu_wikisim_contacts): the view's units, deaths and types
     as the host reads them, its types by index; each lit grid's contacts kept beside it for sightContacts */
  const contactsOf = new WeakMap();
  function sightLit(v, S, seat, g) {
    const types = v.game ? v.game.types : [], T = new Map(types.map((t, i) => [t.name, i]));
    const ix = n => T.has(n) ? T.get(n) : -1;
    const a = withHandles(() => pure({ op: 'sight', table: S ? handle('table', S) : null, seat, tick: g, contacts: true, view: {
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
  const sighttable = (S, d) => S ? withHandles(() => pure({ op: 'sighttable', table: handle('table', S), distance: d }))
    : pure({ op: 'sighttable', table: null, distance: d });

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
    sceneOf: (entry, demo) => entry.scenes[withHandles(() => pure({ op: 'scene', entry: entryOf(entry, demo) })).scene],
    // a players entry's seat's camera at a tick (tagpu_wikidata_seat_cam), null for a seat with none
    seatCam: (entry, demo, k, t) => withHandles(() => pure({ op: 'seatcam', entry: entryOf(entry, demo), seat: k, tick: t })).at,
    // the game frame's 4:3 bound on a game entry's map (tagpu_wikidata_frame_fits)
    frameFits: (w, h) => pure({ op: 'fits', w, h }).fits,
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
    Object.defineProperty(api, k, { enumerable: true, get: () => constants()[k] });
  return api;
})();
if (typeof module !== 'undefined') module.exports = WikiSim;
