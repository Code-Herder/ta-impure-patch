/* replay.js -- the in-game wiki's replay layer: a game entry's demo (research/notes/ingame-wiki-replay-plan.md) played
   the way the replay HUD plays a recording. It holds the viewer's clock and the HUD's state, and owns the game
   simulations; sim.js knows nothing of the HUD. tagpu/ddraw/src/tagpu_wikireplay.c is its twin, function for function
   and under its names: tools/wiki/simparity.mjs compares the two demo tick by demo tick, through the state hash of
   each and the replay's own (tagpu_wikireplay.h), and tools/wiki/simcheck.mjs checks every demo's claims.

   TWO CLOCKS (decision 4). The demo's game script is played by a game simulation (WikiSim.game) on game ticks; the
   viewer's script, the demo's `inputs` (captions, the `seeked` wait and the HUD's presses, sim.js checkViewer), runs on
   demo ticks, one a step, and moves the game only through the HUD's controls: paused, a speed, a seek. Try it yourself
   (create) has no viewer's script: the player's presses come through press(), each acting at once.

   LEAVE, THEN YES, ends Try it yourself's replay (decision 23): leave:yes is a press of create's alone, never a
   script's (both readers refuse it in a demo) and does nothing in play's. It closes the question and the replay is done
   and `left`: the page holds its last frame and says that in the game this goes back to the Replays screen.

   THE MARKERS come from the simulation, never from the script (decision 2): before the replay shows, a second game
   simulation of the same demo, the walker, plays the game script to its end WALK ticks a step, and each tick a seat's
   commanders lost rising marks a death and its count of units reaching 0 from above a defeat, as the game's walker reads
   a recording ahead of the playhead (tagpu_hudmarks.c, THE WALKER: +0x106 rising, +0x144 reaching 0). The viewer's
   clock and the game hold meanwhile, as a wait holds a demo's clock.

   A SEEK RE-SIMULATES (decision 11): forward, the game plays on to the target; back, the game is freed and made again
   from the demo and plays from tick 0. Either way the world at the target is the world of the game played straight to
   it, which simcheck's seek claim checks. A seek never asks a view between its ticks.

   THE BOUND: a step runs at most max(SEEK, WALK, the top speed's 8) game ticks -- the walk WALK, a seek SEEK, a play
   SPEEDS' last / 10 -- whatever the script or the presses, so a step's cost has a ceiling by construction, not by how
   fast a tick happens to be. cost() is the ticks the step will run as the state stands; a seek the step's own viewer
   inputs start adds at most SEEK to it.

   THE LIFETIME: the replay owns its game and walker simulations and is the only code that frees them -- the old game at
   a backward seek before the new one is made, the walker at the walk's end, both in free() -- and keeps no other
   reference to either: R.game is replaced the moment its sim is freed. Each frees its COB world with it, so a replay
   holds at most two live worlds and the scripts' host's drop of the oldest of its 64 (serve.py) never takes a live one.

   THE SOUND: a seek's re-simulated events are heard already. The view's `quiet` is the game's sound total when the last
   seek ended (0 for a new simulation, which `gen` names), and the page plays only the events past it.

   LIVE (a live entry, `"live": true`: research/notes/ingame-wiki-replay-plan.md, entry 4). The demo plays a live
   skirmish with the live HUD's rewind instead of a replay, the game's own rewind state for state with its waits
   removed (tagpu_hudinput.c live_act, live_play, live_fill; rewind-spec.md section 7). There is no walker: the live
   HUD has no markers. The live simulation L (R.game) plays the selected timeline a tick a step while the rewind is
   idle and is paused while the strip is open or the past is shown; the past is a second simulation P (R.past) that
   re-plays the selected timeline's CHAIN from tick 0 -- the original to its first fork, each fork from its start R --
   applying each chain member's fork operation (sim.fork) when it reaches that member's R, and each member's logged
   inputs and releases between the same two ticks they were applied live. Play from here asks the plan where a fork
   would start: the last tick at or before the one asked for that the recorder's writer flags a refresh
   (reclossy_is_refresh, isRefresh below), never the segment's first, and which fork the recorder would evict to keep
   FORK_LIMIT live forks (rec_fork_evict, forkEvict below); the first click snaps the past back to that start and arms,
   the second forks there. Back to now shows L as it was, then lets go of seat 0's input as the game's rebuild does
   (sim.release, m0-facts.md) and logs that release, so every later re-simulation applies it at the same point.

   THE LIFETIME, LIVE: the replay owns L and P and is the only code that frees them: P at a backward seek before the
   new one is made, at Back to now, and at the end; L at a fork, the moment P takes its place, and at the end. R.game is
   replaced the moment its simulation is freed. L is never stepped or written while the past is shown, so Back to now
   shows its world exactly as it was; the release that follows is the game's own (m0-facts.md) and is logged like an
   input.

   THE BOUND, LIVE: a step runs at most max(SEEK, 1) game ticks: one of L's while idle, SEEK of P's while a seek is
   pending, and one fork operation per chain member P crosses. The timelines are at most TIMELINES (the recorder's
   RECR_TIMELINES: the plan refuses the one past it); the logs hold at most LOG entries over every kept log, and a log
   is freed when its timeline is deleted and no live timeline's chain reads it. LOG_KEEP of them are kept back while
   the rewind is idle -- one key's up per key the world can hold, and the release -- so that leaving idle and Back to
   now can always log what they apply.

   LOGGED FIRST: Try it yourself's world input after a fork is written to the selected timeline's log and then applied,
   through the one function a re-simulation applies it with; the log is the fork's script, and an input applied but
   not logged would be a world no seek can make again. One that cannot be logged is not applied.

   ONE CURSOR (the clash rule): in a scripted live demo the viewer and seat 0 are one hand, so while the viewer's trip
   or its return glide to the game runs and the rewind is idle, L may feed seat 0 no input; a step that does sets
   `clash` and ends the replay, which simcheck fails.

   Runs in a page (the global WikiReplay, after sim.js) and under node (module.exports), as sim.js does. */
const WikiReplay = (() => {
  const Sim = typeof WikiSim !== 'undefined' ? WikiSim : require('./sim.js');
  /* tagpu_wikireplay.h's constants (simparity compares them): the controls a press names (sim.js's HUD_CONTROLS,
     WIKI_HUD_*), the speeds in tenths with the one a replay starts at (hud-in-game.md: 5, 10, 20, 40, 80, 1/2x to 8x),
     the game ticks a seek and the walk play in one step (SEEK, WALK), rew's and fwd's 10 s (STEP), a bookmark's reach
     of 3 s (NEAR), the bookmarks and markers held (BOOKS; MARKS: a death per kill of a commander, a defeat per seat),
     the most game ticks a page's fill lets a replay run (FILL, the presentation's), and a press's trip (HUD_DOWN,
     HUD_UP: the popup pick's without its glide back); PRESSES, what Try it may press: the replay HUD's controls and
     the question's Yes (WIKI_HUD_LEAVE_YES), never a live HUD's */
  const CONTROLS = Sim.HUD_CONTROLS, HUD_DOWN = Sim.HUD_DOWN, HUD_UP = Sim.HUD_UP, PRESSES = [...Sim.REPLAY_CONTROLS, 'leave:yes'];
  const SPEEDS = [5, 10, 20, 40, 80], SPEED0 = 10;
  // SEEK, WALK and FILL are set from the tick's cost in the game: tagpu_wikireplay.h states the measurement
  const SEEK = 64, WALK = 64, STEP = 300, NEAR = 90, BOOKS = 64, MARKS = Sim.GAME_KILLS + Sim.SEATS, FILL = 64;
  // a marker's kind, tagpu_hudlay.h's HUD_MK_DEATH and HUD_MK_DEFEAT; a toast's, WIKI_TOAST_*
  const DEATH = 0, DEFEAT = 1, TOASTS = ['added', 'removed', 'playing_on', 'cannot', 'armed', 'playing_from', 'refused'];
  /* THE LIVE MODE's names and bounds (tagpu_wikireplay.h WIKI_RW_*, WIKI_ARM_*, WIKI_REPLAY_LOG and _LOG_KEEP): the
     rewind's states and Play from here's arm; the span's first GameTime (a recording's first iteration is GameTime 1,
     demo-recorder-s4-rewind.md); the DisplayTimer's step, 30 GameTimes (engine/render.md, The memo's layout); the logs'
     bound and what the idle rewind keeps back of it; a trip's start at seat 0's cursor */
  const RW = ['idle', 'open', 'past'], ARMS = ['none', 'seeking', 'armed'], FIRST = 1, RATE = 30;
  const LOG = 65536, LOG_KEEP = Sim.KEYS.length + 1, AT_GAME = '@game';

  /* THE RECORDER'S RULES, the twins of tagpu/ddraw/inc/tagpu_recrules.h, which the recorder runs and the DLL's replay
     layer includes (simparity compares them case by case): the refresh interval a recording is written with
     (REC_REFRESH_TICKS), the live forks a recording keeps (REC_FORK_LIMIT) and the segments a file may hold
     (RECR_TIMELINES); the refresh grid (refresh_bin, reclossy_is_refresh) and the eviction (rec_fork_evict). */
  const REFRESH_TICKS = 300, FORK_LIMIT = 3, TIMELINES = 64;
  // the interval of the refresh grid that holds GameTime t: 1 to interval is the first
  const refreshBin = (t, interval) => Math.floor((t + interval - 1) / interval);
  /* whether a segment's iteration at GameTime `tick` is a refresh, `n` its ordinal in the segment from 1 and `prev` the
     GameTime of the segment's last refresh before it: the first two, then the first in each interval other than the last
     refresh's; `interval` 0 makes only the first two */
  const isRefresh = (n, tick, prev, interval) =>
    n <= 2 || (interval !== 0 && refreshBin(tick, interval) !== refreshBin(prev, interval));
  /* the recorder's choice among `rows`, [{id, live, used}] in file order (id 0 the original): never the original, never
     the selected; the least recently used, the earliest on a tie; refused when the live set cannot be bounded. Fewer
     than `limit` live forks evict none; exactly `limit` the candidate; more, or `limit` with no candidate, cannot be
     bounded. {ok: true, evict: the id to delete or null for none}, or {ok: false, evict: null} */
  function forkEvict(rows, selected, limit) {
    let live = 0, oldest = Infinity, evict = null;
    for (const r of rows)
      if (r.id && r.live) {
        live++;
        if (r.id !== selected && (evict === null || r.used < oldest)) { oldest = r.used; evict = r.id; }
      }
    if (live < limit) return { ok: true, evict: null };
    if (evict === null || live > limit) return { ok: false, evict: null };
    return { ok: true, evict };
  }

  /* `scripted`: true to play the viewer's script (play), false for Try it yourself (create). The game plays in refuse
     mode in both: Try it of a replay presses the HUD only, so its game is still the recording (decision 18); a live
     entry's Try it forks into stall mode (THE FORK). */
  function make(entry, demo, layout, ctx, scripted) {
    const R = { tick: 0, game: Sim.game(entry, demo, layout, ctx, 'refuse'), past: null };
    const LIVE = entry.frame === 'game' && entry.live === true;
    // the walker makes no sound: nothing of it is heard. A live replay has none: the live HUD has no markers
    let walker = null;
    if (!LIVE) {
      try { walker = Sim.game(entry, demo, layout, ctx == null ? ctx : { ...ctx, snd: null }, 'refuse'); }
      catch (e) { R.game.free(); throw e; }
    }
    const end = demo.game.end;
    // the viewer's script in the order it is fed, by tick then file order; its end, never for Try it
    const script = scripted ? (Array.isArray(demo.inputs) ? demo.inputs : []).map((e, i) => [e, i])
      .sort((a, b) => a[0].at - b[0].at || a[1] - b[1]).map(p => p[0]) : [];
    const vend = scripted && demo.end != null ? demo.end : Infinity;
    let clock = 0, next = 0, wait = null, stalled = false, caption = null, failed = false, ignored = 0, done = false;
    let paused = false, speed = SPEED0, acc = 0, target = -1, gen = 1, quiet = 0;
    let books = [], toast = null, question = null, left = false;
    /* the viewer's cursor: the last trip, {from, fromValue, to, value, at, drag}, from the previous trip's control and
       its final value, the track's tick (-1 for a button), the clock tick it began and the drag's {drag, over}; `pos`
       the track's position, the last tick it sought, which the next trip starts from */
    let cursor = null, pos = -1;
    // the walk: the markers in the order made, the walker's tick, and each seat's last counts
    const marks = [];
    let walked = 0, prev = R.game.gameNow().seats.map(() => ({ lost: 0, out: false }));
    /* THE LIVE STATE (tagpu_wikireplay.h, WIKIREPLAY's live part): the rewind's state, the GameTime it opened at and the
       span's last; the track's last seek (-1 after each other press, as the HUD's press resets it); the clash; the
       clock tick of the return glide's start (-1 none); the past simulation's generation, its chain (row indices from
       the original), the member it plays and that member's next log entry; Play from here's arm, its start and the fork
       it evicts (-1 none); the timelines in creation order, a row's index its id (ids only grow, a deleted row stays),
       the selected one, the highest id made and the entries held in every log; the top bar's memo (the eased stored
       energy and metal and the four rates, f32 as the engine's), the shown record's DisplayTimer and the shown world's
       figures it eases toward; `made` numbers every simulation the replay makes, `why` a failure's reason */
    let rw = 'idle', G = 0, last = 0, dragT = -1, clash = false, back = -1;
    let pgen = 0, pchain = [], pm = 0, pl = 0, arm = 'none', armR = 0, armEvict = -1;
    const rows = LIVE ? [{ id: 0, parent: -1, R: FIRST, last: 0, live: true, used: 0, mode: 'refuse', part: null, log: [] }] : [];
    let selected = 0, nlogged = 0, made = 1, why = null;
    let be = 0, bm = 0, rates = [0, 0, 0, 0], dt = 0, shown = null;

    const sound = () => R.game.gameNow().sound;
    const stopped = () => R.game !== null && R.game.gameNow().stopped;
    const isDone = () => clock >= vend || stalled || failed || left || clash || stopped();
    // a seat's lost rising, a death; its out set, a defeat. BOUND: MARKS (above), and the guard holds it whatever.
    function publish() {
      walker.gameNow().seats.forEach((z, k) => {
        if (z.lost > prev[k].lost && marks.length < MARKS) marks.push([walker.tick, DEATH, k]);
        if (z.out && !prev[k].out && marks.length < MARKS) marks.push([walker.tick, DEFEAT, k]);
        prev[k] = { lost: z.lost, out: z.out };
      });
    }
    function endWalk() { walked = walker.tick; walker.free(); walker = null; }

    /* THE SEEK, to T clamped to the game: back, a new game; forward, the target; to where it stands, none. Ending a
       seek in progress there makes what it played heard. */
    function seek(T) {
      T = Math.min(Math.max(T, 0), end);
      if (T < R.game.tick) {
        /* THE LIFETIME: the old game is freed before the new one is made, and R.game is cleared with it, so no code can
           reach the freed sim; nothing else holds it (the view is made afresh from R.game at each call). */
        R.game.free();
        R.game = null;
        try { R.game = Sim.game(entry, demo, layout, ctx, 'refuse'); } catch (e) { failed = true; return; }
        gen++;
        quiet = 0;
        target = T === 0 ? -1 : T;
      } else if (T > R.game.tick) target = T;
      else {
        if (target >= 0) quiet = sound();
        target = -1;
      }
      acc = 0;
    }
    /* THE CONTROLS, as the game's HUD acts (hud-in-game.md, the keys' table). While Leave's question is open every
       press but its two answers is ignored, and an answer with none open is, and leave:yes in a replay that plays a
       script, and a live HUD's control, which a replay has none of; each ignored press is counted. */
    function act(c, value) {
      if (failed) return;
      if (LIVE) { liveAct(c, value); return; }
      const answer = c === 'leave:no' || c === 'leave:yes';
      if (question ? !answer : answer) { ignored++; return; }
      if (c === 'leave:yes') {
        if (scripted) { ignored++; return; }
        question = null;
        left = true;
        return;
      }
      const playhead = target >= 0 ? target : R.game.tick;
      if (c === 'play') {
        if (playhead === end) { seek(0); paused = false; } else paused = !paused;
      } else if (c === 'speed') {
        speed = SPEEDS.find(s => s > speed) ?? SPEEDS[0];
        acc = 0;
      } else if (c === 'rew') seek(playhead - STEP);
      else if (c === 'fwd') seek(playhead + STEP);
      else if (c === 'track') seek(value);
      else if (c === 'bookmark') {
        // the nearest within NEAR, the earlier on a tie (the list is sorted, and only a nearer one replaces it)
        let k = -1;
        books.forEach((b, i) => { if (Math.abs(b - playhead) <= NEAR && (k < 0 || Math.abs(b - playhead) < Math.abs(books[k] - playhead))) k = i; });
        if (k >= 0) { toast = { kind: 'removed', at: R.tick, value: books[k] }; books.splice(k, 1); }
        else if (books.length < BOOKS) {
          const i = books.findIndex(b => b > playhead);
          books.splice(i < 0 ? books.length : i, 0, playhead);
          toast = { kind: 'added', at: R.tick, value: playhead };
        }
      } else if (c === 'leave') { question = { wasPaused: paused }; paused = true; }
      else if (c === 'leave:no') { paused = question.wasPaused; question = null; }
      else ignored++;
    }
    /* THE TRIP's events on clock tick c: a button acts at its release; the track seeks at its press, during a drag at
       each tick i = 1..over to to + trunc((drag - to) i / over) when that differs from the last, and at the release to
       where it is. The quotient is C's integer division: |numerator| < 2^31, and a non-integer quotient lies at least
       1/over from an integer, far beyond half an ulp. */
    function trip(c) {
      const t = cursor, P = t.at + HUD_DOWN;
      if (t.to !== 'track') { if (c === t.at + HUD_UP) act(t.to); return; }
      if (c === P) {
        // the press on the track starts a new drag: no seek is a repeat of the last one (tagpu_hudinput.c, the press)
        if (LIVE) dragT = -1;
        act('track', t.value);
      }
      if (t.drag && c > P && c <= P + t.drag.over) {
        const p = t.value + Math.trunc(((t.drag.drag - t.value) * (c - P)) / t.drag.over);
        if (p !== pos) { pos = p; act('track', p); }
      }
      if (c === (t.drag ? P + t.drag.over : t.at + HUD_UP)) act('track', pos);
    }
    // the viewer's clock tick c: the trip's events first (no other press stands in a trip), then the script's inputs
    function feed(c) {
      if (cursor) trip(c);
      while (next < script.length && script[next].at === c) {
        const e = script[next++];
        if ('until' in e) wait = { from: R.tick, max: e.max };
        else if ('hud' in e) {
          // in a live demo a trip that starts while the rewind is idle starts from seat 0's cursor in the game
          const fromGame = LIVE && rw === 'idle';
          cursor = { from: fromGame ? AT_GAME : cursor ? cursor.to : null, fromValue: fromGame || !cursor ? -1 : pos, to: e.hud,
            value: e.hud === 'track' ? e.to : -1, at: c, drag: 'drag' in e ? { drag: e.drag, over: e.over } : null };
          pos = cursor.value;
        } else caption = e.caption || null;
      }
    }
    // the game's part of a step: a seek's ticks, else the speed's
    function play() {
      const g = R.game;
      if (target >= 0) {
        const n = Math.min(SEEK, target - g.tick);
        for (let i = 0; i < n && !g.done; i++) g.step();
        if (g.tick === target) { target = -1; quiet = sound(); }
      } else if (!paused) {
        acc += speed;
        while (acc >= 10 && g.tick < end && !g.done) { g.step(); acc -= 10; }
        if (g.tick === end) { paused = true; acc = 0; }
      }
    }


    /* ---- THE LIVE MODE (the header's LIVE) ---- */
    const releaseOf = t => t.drag ? t.at + HUD_DOWN + t.drag.over : t.at + HUD_UP;
    // the viewer's hand is busy on clock tick c: a trip from its start to its release, or the glide back to the game
    const inHand = c => (cursor && c >= cursor.at && c <= releaseOf(cursor)) || (back >= 0 && c >= back && c < back + HUD_DOWN);
    // the GameTime the world shown stands at: G while open; the seek's T - 1, or P's tick, in the past; L's while idle
    const nowTick = () => rw === 'open' ? G : rw === 'past' ? (target >= 0 ? target - 1 : R.past ? R.past.tick : G) : R.game.tick;
    // a timeline's chain: the row indices from the original down to it (a row's index is its id)
    function chainOf(id) {
      const c = [];
      for (let k = id; k >= 0; k = rows[k].parent) c.unshift(k);
      return c;
    }
    // a log entry applied to a simulation, by the call that applied it live: an input, or the release
    const apply = (s, x) => x.kind === 'release' ? s.release() : s.input(x.kind === 'move' ? { move: [x.x, x.z] } : { [x.kind]: x.key });
    // LOGGED FIRST (the header): the entry joins the selected timeline's log, then L takes it; the callers keep the bound
    function logApply(x) {
      x.tau = R.game.tick + 1;
      rows[selected].log.push(x);
      nlogged++;
      apply(R.game, x);
    }
    const toastOf = (kind, value, evict = -1) => { toast = { kind, at: R.tick, value, evict }; };
    /* THE PLAN (tagpu_replay.c takeover_point, tagpu_recreader.c recr_append_point): a fork asked for at GameTime T
       starts in the chain member holding T, at the last GameTime r of the member's segment up to T that the writer
       flags a refresh, walked from the member's first with the ordinal n from 1 (isRefresh, the recorder's own rule);
       the member's first is refused, as is a timeline past TIMELINES and a live fork set the eviction cannot bound.
       {R, parent, evict (-1 none)} or null. At most `end` integer steps: it runs on a click, never per step. */
    function plan(T) {
      const chain = chainOf(selected);
      let k = chain.length - 1;
      while (k > 0 && rows[chain[k]].R > T) k--;
      const m = rows[chain[k]];
      let R0 = m.R;
      for (let r = m.R, n = 1, prevR = 0; r <= T; r++, n++) if (isRefresh(n, r, prevR, REFRESH_TICKS)) { R0 = r; prevR = r; }
      if (R0 === m.R || rows.length >= TIMELINES) return null;
      const e = forkEvict(rows.map(x => ({ id: x.id, live: x.live, used: x.used })), selected, FORK_LIMIT);
      return e.ok ? { R: R0, parent: m.id, evict: e.evict === null ? -1 : e.evict } : null;
    }
    // the open: refused when the span is empty (tagpu_replay.c: no complete seek interval yet) or the log cannot keep LOG_KEEP
    function open() {
      const L = R.game;
      if (L.tick - 1 <= FIRST || nlogged > LOG - LOG_KEEP) { toastOf('refused', 0); return; }
      // Try it after a fork: each key and button the world holds goes up into L, logged, so L never holds one the
      // player cannot let go of while the strip is open
      if (!scripted && selected !== 0) for (const k of L.gameNow().keys) logApply({ kind: 'up', key: k, x: 0, z: 0 });
      G = L.tick;
      last = G - 1;
      rw = 'open';
    }
    // the close from the strip open without a seek: the game rebuilds nothing (tagpu_replay.c rw_close_unmoved)
    function close() {
      rw = 'idle';
      back = clock;
    }
    /* BACK TO NOW (and a close in the past, which the game runs as the same rebuild): P freed (THE LIFETIME), then the
       release on L, logged at G + 1 like an input; the DisplayTimer set to G (m0-facts.md, 2561 read at GameTime 2536
       after Back to now at 2501: set to G, then raised at 2502 and 2532 [INFERRED from the arithmetic]) */
    function toNow() {
      if (R.past) { R.past.free(); R.past = null; }
      target = -1;
      logApply({ kind: 'release', key: null, x: 0, z: 0 });
      dt = G;
      quiet = sound();
      rw = 'idle';
      back = clock;
    }
    /* THE LIVE SEEK to T, shown as the world of T - 1: back of P's tick (or with no P), P freed (THE LIFETIME) and made
       again from the demo to re-play the selected timeline's chain from tick 0; forward, the target. A make that throws
       fails the replay, as Watching's. */
    function liveSeek(T) {
      if (!R.past || T - 1 < R.past.tick) {
        if (R.past) { R.past.free(); R.past = null; }
        try { R.past = Sim.game(entry, demo, layout, ctx, 'refuse'); } catch (e) { failed = true; why = e.message; return; }
        pgen = ++made;
        pchain = chainOf(selected);
        pm = 0;
        pl = 0;
      }
      rw = 'past';
      target = T;
    }
    /* ONE TICK OF P along the chain, from t to t + 1: at the next member's R the member's fork operation, then the
       entries the member logged at t + 1, in log order, through the calls that applied them live, so P's world is the
       world the timeline was played with by construction */
    function pastTick() {
      const P = R.past, t = P.tick;
      if (pm + 1 < pchain.length && t + 1 === rows[pchain[pm + 1]].R) {
        const m = rows[pchain[++pm]];
        pl = 0;
        try { P.fork(m.R, m.part || [], m.mode); } catch (e) { failed = true; why = e.message; return; }
      }
      const log = rows[pchain[pm]].log;
      for (; pl < log.length && log[pl].tau <= t + 1; pl++) if (log[pl].tau === t + 1) apply(P, log[pl]);
      P.step();
    }
    // the rows a live timeline's chain reads keep their part and log; every other deleted row frees both (THE BOUND)
    function sweep() {
      const keep = new Set();
      for (const r of rows) if (r.live) for (let k = r.id; k >= 0 && !keep.has(k); k = rows[k].parent) keep.add(k);
      for (const r of rows) if (!keep.has(r.id) && (r.log.length || r.part)) { nlogged -= r.log.length; r.log = []; r.part = null; }
    }
    /* THE FORK, with P standing at p.R - 1 and no seek pending: the part a scripted demo gives the timeline of the next
       id (its `from` and `parent` must be the plan's), or Try it's none in stall mode -- the stall mode's caller; then
       the fork operation, which ends with the release; the selected row closed at G, the evicted one deleted, the new
       row selected, and P takes L's place (THE LIFETIME). The game's fork sets the DisplayTimer to the GameTime shown
       (MEASURED, m0-facts.md: 1200 at the fork that takes over at 1201). */
    function forkAt(p) {
      const P = R.past, id = rows.length;
      let part = [], mode = 'stall';
      if (scripted) {
        const f = (demo.forks || []).find(x => x.id === id);
        if (!f || f.from !== p.R || f.parent !== p.parent) {
          failed = true;
          why = !f ? `no fork part of id ${id}` : `the fork part ${id} is from ${f.from}, parent ${f.parent}; the fork is at ${p.R}, parent ${p.parent}`;
          return;
        }
        part = f.inputs;
        mode = 'refuse';
      }
      try { P.fork(p.R, part, mode); } catch (e) { failed = true; why = e.message; return; }
      dt = p.R - 1;
      rows[selected].last = G;
      if (p.evict >= 0) rows[p.evict].live = false;
      rows.push({ id, parent: p.parent, R: p.R, last: p.R - 1, live: true, used: R.tick, mode, part, log: [] });
      selected = id;
      sweep();
      R.game.free();
      R.game = P;
      R.past = null;
      gen = pgen;
      quiet = sound();
      target = -1;
      rw = 'idle';
      arm = 'none';
      back = clock;
      toastOf('playing_from', p.R);
    }
    /* PLAY FROM HERE (tagpu_hudinput.c live_play): open, it closes and plays on; in the past, armed it forks, else it
       plans at the GameTime after the world shown -- refused, it says so; at a refresh with nothing to evict it forks
       at once; else it arms, snapping the past back to the plan's start when that is not the world shown */
    function playFrom(now) {
      if (rw === 'open') { close(); toastOf('playing_on', G); return; }
      if (rw !== 'past' || arm === 'seeking') return;
      const p = plan(now + 1);
      if (!p) { arm = 'none'; toastOf('cannot', now); return; }
      if (arm === 'armed' || (p.R === now + 1 && p.evict < 0)) { forkAt(p); return; }
      armR = p.R;
      armEvict = p.evict;
      if (p.R !== now + 1) liveSeek(p.R);
      arm = 'seeking';
    }
    /* THE LIVE HUD's CONTROLS (tagpu_hudinput.c live_act, rewind-spec.md 3.2): a control outside LIVE_CONTROLS and Play
       from here while a seek is pending are ignored and counted; every other ends Play from here's arm, and every one
       but the track ends the track's drag */
    function liveAct(c, value) {
      if (!Sim.LIVE_CONTROLS.includes(c) || (c === 'live_play' && target >= 0)) { ignored++; return; }
      if (c !== 'live_play') arm = 'none';
      if (c !== 'track') dragT = -1;
      const now = nowTick();
      if (c === 'rewind') { if (rw === 'idle') open(); else if (rw === 'open') close(); else toNow(); }
      else if (c === 'rew') { if (rw !== 'idle') liveSeek(Math.max(now - STEP, FIRST + 1)); }
      // open, fwd is drawn disabled; in the past a jump that reaches the span's last is Back to now
      else if (c === 'fwd') { if (rw === 'past') { if (now + STEP >= last) toNow(); else liveSeek(now + STEP); } }
      else if (c === 'track') {
        if (rw === 'idle') return;
        const T = Math.min(Math.max(value, FIRST + 1), last);
        if (T !== dragT) { dragT = T; liveSeek(T); }
      } else if (c === 'live_now') { if (rw === 'open') close(); else if (rw === 'past') toNow(); }
      else playFrom(now);
    }
    /* the live game's part of a step: L's tick while idle (the clash rule reads its fed count), a seek's ticks of P in
       the past; the seek ends on T - 1, and its world is the one shown, its DisplayTimer the GameTime */
    function livePlay() {
      const L = R.game;
      if (rw === 'idle') {
        if (L.tick >= end || L.done) return;
        const fed = L.gameNow().fed;
        L.step();
        if (scripted && inHand(clock) && L.gameNow().fed !== fed) clash = true;
      } else if (rw === 'past' && target >= 0 && R.past) {
        const n = Math.min(SEEK, target - 1 - R.past.tick);
        for (let i = 0; i < n && !R.past.done && !failed; i++) pastTick();
        if (!failed && R.past.tick === target - 1) {
          target = -1;
          quiet = R.past.gameNow().sound;
          dt = R.past.tick;
        }
      }
    }
    // THE ARM's fill (tagpu_hudinput.c live_fill): out of the past it ends; the snap's seek done, it holds; moved, it ends
    function armFill() {
      if (rw !== 'past') arm = 'none';
      else if (arm === 'seeking' && target < 0 && R.past.tick + 1 === armR) { arm = 'armed'; toastOf('armed', armR, armEvict); }
      else if (arm === 'armed' && R.past.tick + 1 !== armR) arm = 'none';
    }
    /* THE TOP BAR's FRAME, DrawGameScreen's memo at main+0x37E3F, one frame a step (DISASSEMBLED, engine/render.md, The
       memo's layout): each stored value eased toward the shown world's, d = ftol(stored) - ftol(old) (0x4E43A0 sets the
       FPU to chop before fistp: truncation), a step of d / 8 truncated toward zero and at least 1 either way
       (0x468E7B..0x468F12), then held to the storage with no test of it being above 0 (0x468F14..0x468F6C); then once,
       when the shown record's DisplayTimer +0xF8 is below the GameTime, it rises by 30 and the four rates are read
       again (0x468F73..0x468FBF). MEASURED by M0 (m0-facts.md, 2026-11-01): the DisplayTimer is 30 at GameTime 1, 60
       from 31, 90 from 61; the memo persists through a seek and a fork; in the past the rates are the ones sampled
       before the open (+25/-0 energy shown at 699 while the record held 50.15 of expense). The shown world is L's, or
       P's once its seek has ended; while a seek is pending, the last world drawn, which the stage holds. */
    function barFrame() {
      if (!(rw === 'past' && (target >= 0 || !R.past))) {
        const s = rw === 'past' ? R.past : R.game;
        shown = { tick: s.tick, ...s.gameNow().bar };
      }
      const ease = (old, stored, cap) => {
        const o = Math.trunc(old), d = Math.trunc(stored) - o;
        const step = Math.trunc(d / 8) || Math.sign(d), v = o + step;
        return Math.fround(v > cap ? cap : v);
      };
      be = ease(be, shown.e, shown.emax);
      bm = ease(bm, shown.m, shown.mmax);
      if (dt < shown.tick) {
        dt += RATE;
        rates = [shown.ei, shown.eo, shown.mi, shown.mo].map(Math.fround);
      }
    }

    /* ONE STEP, a demo tick: while the walk runs, WALK of its ticks and nothing else; then the viewer's clock (a held
       wait releases when no seek is pending and stalls past its max) and the game's part; live, the arm's fill and the
       top bar's frame after it */
    R.step = () => {
      if (done) return;
      R.tick++;
      if (walker) {
        const n = Math.min(WALK, end - walker.tick);
        for (let i = 0; i < n && !walker.done; i++) { walker.step(); publish(); }
        if (walker.done) endWalk(); else walked = walker.tick;
        done = isDone();
        return;
      }
      if (wait && target < 0) wait = null;
      else if (wait && R.tick - wait.from > wait.max) stalled = true;
      if (!wait) feed(++clock);
      if (LIVE) {
        if (!failed) livePlay();
        if (!failed) { armFill(); barFrame(); }
      } else if (!failed) play();
      done = isDone();
    };
    // the game ticks the next step runs as the state stands (THE BOUND)
    R.cost = () => {
      if (done) return 0;
      if (LIVE) return rw === 'idle' ? (R.game.tick < end && !R.game.done ? 1 : 0)
        : rw === 'past' && target >= 0 && R.past ? Math.min(SEEK, target - 1 - R.past.tick) : 0;
      if (walker) return Math.min(WALK, end - walker.tick);
      if (target >= 0) return Math.min(SEEK, target - R.game.tick);
      return paused ? 0 : Math.min(Math.floor((acc + speed) / 10), end - R.game.tick);
    };
    /* Try it yourself's press, acting at once: one of PRESSES, live one of LIVE_CONTROLS (anything else does nothing),
       for the track the game tick under the pointer, an integer the seek clamps to the game (live, to the rewind's
       span). None reaches a replay done or still walking, which the page does not show yet. */
    R.press = (c, value) => {
      if (done || walker || !(LIVE ? Sim.LIVE_CONTROLS : PRESSES).includes(c)) return;
      act(c, value);
      done = isDone();
    };
    /* TRY IT YOURSELF'S WORLD INPUT, live (rewind-spec.md 7.8): {move: [x, z]}, {down: key} or {up: key}, given between
       two steps. The world takes it only after a fork (the original is the recording), every one while the rewind is
       idle and a move alone while the strip is open (the game draws the armed building's ghost at the pointer then,
       MEASURED m0-facts.md #1), none in the past; and only while the log can hold it, keeping LOG_KEEP back while idle
       and one entry, the release's, while open (THE BOUND). LOGGED FIRST. Anything else does nothing. */
    R.input = ev => {
      if (!LIVE || scripted || done || selected === 0 || rw === 'past' || !ev) return;
      let x = null;
      if (Array.isArray(ev.move)) x = { kind: 'move', key: null, x: ev.move[0], z: ev.move[1] };
      else if (Sim.KEYS.includes(ev.down)) x = { kind: 'down', key: ev.down, x: 0, z: 0 };
      else if (Sim.KEYS.includes(ev.up)) x = { kind: 'up', key: ev.up, x: 0, z: 0 };
      if (!x || (rw === 'open' && x.kind !== 'move') || nlogged >= (rw === 'open' ? LOG - 1 : LOG - LOG_KEEP)) return;
      logApply(x);
      done = isDone();
    };
    /* THE REPLAY'S VIEW (tagpu_wikireplay.h WIKIREPLAYVIEW): the demo tick, the viewer's clock and its state, the game's
       transport, the cursor and the control held down (from the press to the release), the markers and the walk, the
       bookmarks, the toast, the question and whether Yes left the replay. R.game.view() is the world. */
    R.view = () => {
      const g = R.game, t = cursor;
      const release = t && (t.drag ? t.at + HUD_DOWN + t.drag.over : t.at + HUD_UP);
      const v = {
        tick: R.tick, clock, done, waiting: wait ? 'seeked' : null, stalled, caption, failed, ignored,
        game: LIVE ? { tick: g.tick, end, playhead: nowTick(), paused: rw !== 'idle' || g.tick >= end || g.done, speed, acc, target, gen, quiet }
          : { tick: g ? g.tick : 0, end, playhead: target >= 0 ? target : g ? g.tick : 0, paused, speed, acc, target, gen, quiet },
        cursor: t ? { from: t.from, fromValue: t.fromValue, to: t.to, value: t.value, at: t.at,
          drag: t.drag ? { drag: t.drag.drag, over: t.drag.over } : null } : null,
        held: t && clock >= t.at + HUD_DOWN && clock < release ? t.to : null,
        marks: marks.map(m => m.slice()), walked,
        books: books.slice(), toast: toast ? { ...toast } : null,
        question: question ? { wasPaused: question.wasPaused } : null, left,
      };
      /* LIVE: the rewind (rewind-spec.md 7.10): its state, the GameTime it opened at, the span, the world shown; the arm,
         its start and the fork it evicts (-1 none); the track's last seek; the clash; the entries the logs hold; the
         timelines in creation order and the selected one; the past simulation; the top bar's memo and DisplayTimer;
         the return glide's start; whether the logs hold all the wiki keeps of Try it's input */
      if (LIVE) v.live = {
        rw, G, first: FIRST, last, now: nowTick(), arm, armR, armEvict, dragT, clash, nlogged, selected,
        timelines: rows.map(r => ({ id: r.id, parent: r.parent, R: r.R, last: r.last, live: r.live, used: r.used, mode: r.mode, nlog: r.log.length })),
        past: R.past ? { tick: R.past.tick, target, gen: pgen } : null,
        bar: { be, bm, rates: rates.slice(), dt }, back, full: nlogged >= LOG - LOG_KEEP,
      };
      return v;
    };
    // why the replay failed, or null: a fork part the viewer's fork found wrong, a refused fork operation, a make that threw
    R.why = () => why;
    /* a live replay's timelines as a re-simulation reads them, copies in creation order: each one's id, parent, R, mode,
       part (the fork part's inputs, null for the original) and log; a check plays a chain straight from them
       (simcheck.mjs, the seek and now claims). [] for a replay. */
    R.timelines = () => rows.map(r => ({ id: r.id, parent: r.parent, R: r.R, mode: r.mode, part: r.part, log: r.log.map(x => ({ ...x })) }));
    // frees every simulation it holds; the replay is not used after
    R.free = () => {
      if (R.game) R.game.free();
      if (walker) walker.free();
      if (R.past) R.past.free();
      R.game = null;
      R.past = null;
      walker = null;
    };

    // the seats as the scene and the script's tick 0 leave them, a walk that has nothing to play, the clock's tick 0
    if (!LIVE) {
      publish();
      if (walker.done) endWalk();
    } else {
      /* the memo before the level's first frame: seat 0's stored values (its settings), no rate read, the DisplayTimer
         0 as in a record no frame has drawn (MEASURED, m0-facts.md #5) [INFERRED: the memo before a level's first frame
         is not measured; every read at GameTime 1 found the stored values, which this start shows at once] */
      shown = { tick: 0, ...R.game.gameNow().bar };
      be = Math.fround(shown.e);
      bm = Math.fround(shown.m);
    }
    feed(0);
    done = isDone();
    return R;
  }

  return {
    play: (entry, demo, layout, ctx) => make(entry, demo, layout, ctx, true),
    create: (entry, demo, layout, ctx) => make(entry, demo, layout, ctx, false),
    CONTROLS, PRESSES, SPEEDS, SPEED0, SEEK, WALK, STEP, NEAR, BOOKS, MARKS, FILL, HUD_DOWN, HUD_UP, DEATH, DEFEAT, TOASTS,
    GAME_END_MAX: Sim.GAME_END_MAX, REFRESH_TICKS, FORK_LIMIT, TIMELINES, refreshBin, isRefresh, forkEvict,
    RW, ARMS, FIRST, RATE, LOG, LOG_KEEP, AT_GAME,
  };
})();
if (typeof module !== 'undefined') module.exports = WikiReplay;
