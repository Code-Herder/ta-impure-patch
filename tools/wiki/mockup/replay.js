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

   TAKE OVER (a takeover entry, `"takeover": true`: research/notes/ingame-wiki-replay-plan.md, entry 3). The demo plays
   a replay with the replay HUD's Take over, the game's own state for state with its waits removed (tagpu_hudinput.c
   pick_act, pick_fill, lane_act, lane_fill; takeover-spec.md section 6). WATCH MODE plays the selected timeline's
   chain: the original, or a fork the file holds (the demo's `kept`, or one a deletion left) from its R, each member's
   fork operation applied when the game reaches its R (chainStep), to the chain's last. The picker runs as pick_fill:
   Take over pauses and asks; the plan (the recorder's refresh grid and eviction, as Play from here's, one tick on when
   T is a segment's first) snaps the playback back to R - 1, and the picker opens there, closing when the playhead
   leaves it, when the selected timeline changes, or, planned again, when a deletion moves its R. A lane switches the
   selected timeline: RE-CHAINED when the chain members the game has crossed are the same on both chains -- the world
   at that tick was made by the same fork operations and the game's script is the last such member's, so it is the new
   chain's world by construction and the game is kept -- else SOUGHT, the game made again and played along the new
   chain to the playhead clamped to it. Save as a replay writes a fork once (the export's temp is moved without
   replacing, tagpu_recreader.c, so a second export fails); Delete asks twice, and deleting the watched fork first
   switches to the nearest live timeline up its chain (tagpu_replay.c fork_fallback). A row taken in the open picker
   forks the game at R as that row's seat h and turns the replay into LIVE MODE, Rewind's live game with seat h as the
   human: from then every function of THE LIVE MODE below runs, with seat h where Rewind reads seat 0.

   THE BOUND, WATCH: a switch restarts the walker on the new chain without holding the clock (the game's walk restarts
   with the markers blank, tagpu_hudmarks.c); a step then runs the game's part first and gives the walker what is
   left of WALK, so a step still runs at most max(SEEK, WALK, 8) game ticks, plus one fork operation per chain member
   crossed.

   THE LIFETIME, WATCH: a sought switch frees the game before the new one is made, as a backward seek does, and a
   switch frees the walker before the new one is made; R.game is replaced the moment its simulation is freed. The
   takeover frees the walker: live mode has none.

   Runs in a page (the global WikiReplay, after sim.js) and under node (module.exports), as sim.js does. */
const WikiReplay = (() => {
  const Sim = typeof WikiSim !== 'undefined' ? WikiSim : require('./sim.js');
  /* tagpu_wikireplay.h's constants (simparity compares them): the controls a press names (sim.js's HUD_CONTROLS,
     WIKI_HUD_*), the speeds in tenths with the one a replay starts at (hud-in-game.md: 5, 10, 20, 40, 80, 1/2x to 8x),
     the game ticks a seek and the walk play in one step (SEEK, WALK), rew's and fwd's 10 s (STEP), a bookmark's reach
     of 3 s (NEAR), the bookmarks and markers held (BOOKS; MARKS: a death per kill of a commander, a defeat per seat),
     the most game ticks a page's fill lets a replay run (FILL, the presentation's), and a press's trip (HUD_DOWN,
     HUD_UP: the popup pick's without its glide back); PRESSES, what Try it may press: the replay HUD's controls and
     the question's Yes (WIKI_HUD_LEAVE_YES), never a live HUD's; a takeover entry's watch mode its Take over set too */
  const CONTROLS = Sim.HUD_CONTROLS, HUD_DOWN = Sim.HUD_DOWN, HUD_UP = Sim.HUD_UP, PRESSES = [...Sim.REPLAY_CONTROLS, 'leave:yes'];
  const TK_PRESSES = [...PRESSES, ...Sim.TAKEOVER_CONTROLS];
  const SPEEDS = [5, 10, 20, 40, 80], SPEED0 = 10;
  // SEEK, WALK and FILL are set from the tick's cost in the game: tagpu_wikireplay.h states the measurement
  const SEEK = 64, WALK = 64, STEP = 300, NEAR = 90, BOOKS = 64, MARKS = Sim.GAME_KILLS + Sim.SEATS, FILL = 64;
  // a marker's kind, tagpu_hudlay.h's HUD_MK_DEATH and HUD_MK_DEFEAT; a toast's, WIKI_TOAST_*
  const DEATH = 0, DEFEAT = 1, TOASTS = ['added', 'removed', 'playing_on', 'cannot', 'armed', 'playing_from', 'refused',
    'taking_over', 'cannot_take', 'saved', 'cannot_save', 'click_delete', 'deleted'];
  // the picker's states (tagpu_hudinput.c PICK_*, tagpu_wikireplay.h WIKI_PICK_*) and a takeover replay's modes
  const PICKS = ['closed', 'asked', 'seeking', 'open'], MODES = ['watch', 'live'];
  /* THE LIVE MODE's names and bounds (tagpu_wikireplay.h WIKI_RW_*, WIKI_ARM_*, WIKI_REPLAY_LOG and _LOG_KEEP): the
     rewind's states and Play from here's arm; the span's first GameTime (a recording's first iteration is GameTime 1,
     demo-recorder-s4-rewind.md); the DisplayTimer's step, 30 GameTimes (engine/render.md, The memo's layout); the logs'
     bound and what the idle rewind keeps back of it; a trip's start at seat 0's cursor */
  const RW = ['idle', 'open', 'past'], ARMS = ['none', 'seeking', 'armed'], FIRST = 1, RATE = 30;
  const LOG = 65536, LOG_KEEP = Sim.KEYS.length + 1, AT_GAME = '@game';

  /* THE RECORDER'S RULES, the twins of tagpu/ddraw/inc/tagpu_recrules.h (sim.js holds the refresh grid, which its
     loader's kept forks are held to; simparity compares them case by case): the refresh interval, the live forks a
     recording keeps and the segments a file may hold (RECR_TIMELINES); the refresh grid and the eviction
     (rec_fork_evict). */
  const { REFRESH_TICKS, FORK_LIMIT, refreshBin, isRefresh, refreshAt } = Sim, TIMELINES = Sim.TIMELINES;
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
    // TK, a takeover entry's replay; LIVE, a live game: a live entry's from the start, a takeover entry's from its takeover
    const TK = entry.frame === 'game' && entry.takeover === true;
    let LIVE = entry.frame === 'game' && entry.live === true;
    // the walker makes no sound: nothing of it is heard. A live replay has none: the live HUD has no markers
    const newWalker = () => Sim.game(entry, demo, layout, ctx == null ? ctx : { ...ctx, snd: null }, 'refuse');
    let walker = null;
    if (!LIVE) {
      try { walker = newWalker(); }
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
    /* the viewer's cursor: the last trip, {from, fromValue, fromWatched, to, value, watched, at, drag}, from the previous
       trip's control, its final value and its `watched`, the track's tick (-1 for a button), the timeline watched at its
       press, the clock tick it began and the drag's {drag, over}; `pos` the track's position, the last tick it sought,
       which the next trip starts from. `watched` is the timeline selected at the trip's start, which is the one at its
       press: in watch mode only a press switches the timeline (a lane, Back to main recording, a deletion, a takeover),
       and a trip's own is the one press in hand. Back to main recording is drawn only on the watched lane, which its
       press leaves, so the cursor resting on it, and the next trip leaving it, are placed on the HUD drawn at that
       press (frame.js pressed) */
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
    const rows = LIVE ? [{ id: 0, parent: -1, R: FIRST, last: 0, live: true, used: 0, mode: 'refuse', part: null, log: [], seat: 0 }] : [];
    let selected = 0, nlogged = 0, made = 1, why = null;
    let be = 0, bm = 0, rates = [0, 0, 0, 0], dt = 0, shown = null;
    /* THE TAKE OVER STATE (TK; tagpu_wikireplay.h, WIKIREPLAY's takeover part): the seat the live game's human plays
       (Rewind's 0; a takeover's from its takeover, -1 while watching); the picker's state and its plan -- R, the fork
       it evicts (-1 none), the parent, and the timeline and the rows' serial it was made on; the rows' serial, raised
       by a deletion; the lanes open; the fork armed for deletion (0 none); the forks saved, each once; whether the
       walker walks in the background, after a switch; whether the takeover's picker drew the fork limit's warning.
       The rows (`rows`) are the original, its last the game's end, then each kept fork as it was played. */
    let human = TK ? -1 : 0, pick = 'closed', pickR = 0, pickEvict = -1, pickParent = 0, pickTl = 0, pickSerial = 0;
    let serial = 0, lanes = 0, armed = 0, bg = false, tkFull = false;
    const saved = [];
    if (TK) {
      rows.push({ id: 0, parent: -1, R: FIRST, last: end, live: true, used: 0, mode: 'refuse', part: null, log: [], seat: 0 });
      for (const f of demo.kept || [])
        rows.push({ id: f.id, parent: f.parent, R: f.from, last: f.end, live: true, used: 0, mode: 'refuse', part: f.inputs, log: [], seat: f.seat });
    }
    // the selected chain's last tick: a takeover's watch mode plays its selected timeline to that timeline's last
    const chainLast = () => TK ? rows[selected].last : end;

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
    function endWalk() { walked = walker.tick; walker.free(); walker = null; bg = false; }

    /* THE SEEK, to T clamped to the game: back, a new game; forward, the target; to where it stands, none. Ending a
       seek in progress there makes what it played heard. */
    function seek(T) {
      T = Math.min(Math.max(T, 0), chainLast());
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
      if (TK && (pickAct(c, value) || laneAct(c, value))) return;
      if (c === 'play') {
        if (playhead === chainLast()) { seek(0); paused = false; } else paused = !paused;
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
      if (t.to !== 'track') { if (c === t.at + HUD_UP) act(t.to, t.value); return; }
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
          // a trip's value: the track's tick, a picker row's seat, a lane's fork, -1 for any other button
          cursor = { from: fromGame ? AT_GAME : cursor ? cursor.to : null, fromValue: fromGame || !cursor ? -1 : pos,
            fromWatched: fromGame || !cursor ? 0 : cursor.watched, to: e.hud,
            value: e.hud === 'track' ? e.to : e.hud === 'takeas' ? e.seat : 'fork' in e ? e.fork : -1, watched: selected, at: c,
            drag: 'drag' in e ? { drag: e.drag, over: e.over } : null };
          pos = cursor.value;
        } else caption = e.caption || null;
      }
    }
    // the game's part of a step: a seek's ticks, else the speed's, along the selected chain; the ticks it ran
    function play() {
      const g = R.game;
      let ran = 0;
      if (target >= 0) {
        const n = Math.min(SEEK, target - g.tick);
        for (; ran < n && !g.done && chainStep(g); ran++);
        if (!failed && g.tick === target) { target = -1; quiet = sound(); }
      } else if (!paused) {
        acc += speed;
        while (acc >= 10 && g.tick < chainLast() && !g.done && chainStep(g)) { acc -= 10; ran++; }
        if (g.tick === chainLast()) { paused = true; acc = 0; }
      }
      return ran;
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
    // a log entry applied to a simulation, by the call that applied it live: an input of its seat's, or its seat's release
    const apply = (s, x) => x.kind === 'release' ? s.release(x.seat)
      : s.input(x.kind === 'move' ? { move: [x.x, x.z], seat: x.seat } : { [x.kind]: x.key, seat: x.seat });
    // LOGGED FIRST (the header): the entry, the human seat's, joins the selected timeline's log, then L takes it; the
    // callers keep the bound
    function logApply(x) {
      x.tau = R.game.tick + 1;
      x.seat = human;
      rows[selected].log.push(x);
      nlogged++;
      apply(R.game, x);
    }
    // a toast; `seat` names a picker row's seat (Taking over NAME), -1 none
    const toastOf = (kind, value, evict = -1, seat = -1) => { toast = { kind, at: R.tick, value, evict, ...(seat >= 0 ? { seat } : {}) }; };
    /* THE PLAN (tagpu_replay.c takeover_point, tagpu_recreader.c recr_append_point): a fork asked for at GameTime T
       starts in the chain member holding T, at the last GameTime r of the member's segment up to T that the writer
       flags a refresh, walked from the member's first with the ordinal n from 1 (isRefresh, the recorder's own rule);
       the member's first is refused, with `next` its second (a refresh: T is the segment's first iteration, and the
       picker plans again there), as is a timeline past TIMELINES and a live fork set the eviction cannot bound.
       {ok, R, parent, evict (-1 none), next (0 none)}. At most `end` integer steps: it runs on a click, never per
       step. */
    function plan(T) {
      const chain = chainOf(selected);
      let k = chain.length - 1;
      while (k > 0 && rows[chain[k]].R > T) k--;
      const m = rows[chain[k]], R0 = refreshAt(m.R, T);
      if (R0 === m.R) return { ok: false, next: m.R + 1 };
      if (rows.length >= TIMELINES) return { ok: false, next: 0 };
      const e = forkEvict(rows.map(x => ({ id: x.id, live: x.live, used: x.used })), selected, FORK_LIMIT);
      return e.ok ? { ok: true, R: R0, parent: m.id, evict: e.evict === null ? -1 : e.evict, next: 0 } : { ok: false, next: 0 };
    }
    // the open: refused when the span is empty (tagpu_replay.c: no complete seek interval yet) or the log cannot keep LOG_KEEP
    function open() {
      const L = R.game;
      if (L.tick - 1 <= FIRST || nlogged > LOG - LOG_KEEP) { toastOf('refused', 0); return; }
      // Try it after a fork: each key and button the world holds goes up into L, logged, so L never holds one the
      // player cannot let go of while the strip is open
      if (!scripted && selected !== 0) for (const k of L.gameNow(human).keys) logApply({ kind: 'up', key: k, x: 0, z: 0 });
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
        pl = 0;
        if (!forkMember(P, rows[pchain[++pm]])) return;
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
    /* the part a scripted demo gives the fork of id `id` -- its `from`, `parent` and seat must be the fork's -- or Try
       it's none in stall mode, the economy's stall mode's caller; null, the replay failed, when the part is not there */
    function partOf(id, R0, parent, seat) {
      if (!scripted) return { part: [], mode: 'stall' };
      const f = (demo.forks || []).find(x => x.id === id);
      if (f && f.from === R0 && f.parent === parent && (f.seat ?? 0) === seat) return { part: f.inputs, mode: 'refuse' };
      failed = true;
      why = !f ? `no fork part of id ${id}` : `the fork part ${id} is from ${f.from}, parent ${f.parent}, seat ${f.seat ?? 0}; ` +
        `the fork is at ${R0}, parent ${parent}, seat ${seat}`;
      return null;
    }
    /* THE FORK, with P standing at p.R - 1 and no seek pending: the part of the next id (partOf), then the fork
       operation as the human seat, which ends with the release; the selected row closed at G, the evicted one
       deleted, the new row selected, and P takes L's place (THE LIFETIME). The game's fork sets the DisplayTimer to the
       GameTime shown (MEASURED, m0-facts.md: 1200 at the fork that takes over at 1201). */
    function forkAt(p) {
      const P = R.past, id = rows.length, q = partOf(id, p.R, p.parent, human);
      if (!q || !forkMember(P, { id, R: p.R, part: q.part, mode: q.mode, seat: human })) return;
      const { part, mode } = q;
      dt = p.R - 1;
      rows[selected].last = G;
      if (p.evict >= 0) rows[p.evict].live = false;
      rows.push({ id, parent: p.parent, R: p.R, last: p.R - 1, live: true, used: R.tick, mode, part, log: [], seat: human });
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
      if (!p.ok) { arm = 'none'; toastOf('cannot', now); return; }
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
        const fed = L.gameNow(human).fed;
        L.step();
        if (scripted && inHand(clock) && L.gameNow(human).fed !== fed) clash = true;
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
        shown = { tick: s.tick, ...s.gameNow(human).bar };
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

    /* ---- TAKE OVER (the header's TAKE OVER) ---- */
    /* A CHAIN MEMBER'S FORK OPERATION on simulation s standing at m.R - 1, as the timeline was made: m's part, played
       as m's seat, in m's mode. Refused -- the replay fails -- when that seat has no unit there (the picker draws such a
       seat's row dimmed, with no hot area: tagpu_hudlay.c) or the fork operation refuses */
    function forkMember(s, m) {
      if (s.gameNow().seats[m.seat].units === 0) {
        failed = true;
        why = `the fork F${m.id} takes over seat ${m.seat}, which has no unit at tick ${m.R - 1}`;
        return false;
      }
      try { s.fork(m.R, m.part || [], m.mode, m.seat); } catch (e) { failed = true; why = e.message; return false; }
      return true;
    }
    /* ONE TICK of simulation s along the selected chain: at a member's R its fork operation first. R strictly increases
       along a chain (a fork starts after its parent's first), so at most one member starts on the tick after s's.
       False, nothing stepped, when that fork operation failed the replay */
    function chainStep(s) {
      if (TK) {
        const chain = chainOf(selected);
        for (let k = 1; k < chain.length; k++)
          if (rows[chain[k]].R === s.tick + 1) { if (!forkMember(s, rows[chain[k]])) return false; break; }
      }
      s.step();
      return true;
    }
    // a fork the file holds: a timeline's id other than the original's, not deleted
    const liveFork = id => Number.isInteger(id) && id >= 1 && id < rows.length && rows[id].live;
    const anyFork = () => rows.some(r => r.id && r.live);
    /* the selected chain's tick the stage's track fills to: the R of its member whose parent is the original, else the
       original's end (tagpu_hudstate.c fill_forks' fill_to) */
    function fillTo() {
      const m = chainOf(selected).find(k => k && rows[k].parent === 0);
      return m ? rows[m].R : end;
    }
    /* THE WALK RESTARTED on the selected chain (THE LIFETIME, WATCH: the old walker freed before the new one is made),
       the markers blank until it has walked, in the background (THE BOUND, WATCH) */
    function restartWalk() {
      if (walker) { walker.free(); walker = null; }
      marks.length = 0;
      prev = R.game.gameNow().seats.map(() => ({ lost: 0, out: false }));
      walked = 0;
      try { walker = newWalker(); } catch (e) { failed = true; why = e.message; return; }
      bg = true;
      publish();
    }
    // the background walk's share of a step that ran `ran` of the game's ticks (THE BOUND, WATCH)
    function bgWalk(ran) {
      if (!bg || !walker) return;
      const n = Math.min(WALK - ran, chainLast() - walker.tick);
      for (let i = 0; i < n && !walker.done && chainStep(walker); i++) publish();
      if (failed) return;
      if (walker.done || walker.tick >= chainLast()) endWalk(); else walked = walker.tick;
    }
    /* THE SWITCH to timeline `id` (tagpu_replay.c, the switch): the timeline left stamped as used, the picker closed;
       RE-CHAINED when the chain members the game has crossed (R <= its tick) are the same on both chains, the game kept
       and a pending seek clamped to the new chain; else SOUGHT to the playhead clamped to the new chain, the game freed
       before the new one is made (THE LIFETIME, WATCH); then the walk restarted on the new chain */
    function switchTo(id) {
      rows[selected].used = R.tick;
      pick = 'closed';
      const t = R.game.tick, crossed = k => chainOf(k).filter(j => rows[j].R <= t).join();
      if (crossed(selected) === crossed(id)) {
        if (target >= 0) target = Math.min(target, rows[id].last);
      } else {
        const T = Math.min(target >= 0 ? target : t, rows[id].last);
        R.game.free();
        R.game = null;
        try { R.game = Sim.game(entry, demo, layout, ctx, 'refuse'); } catch (e) { failed = true; why = e.message; return; }
        gen++;
        quiet = 0;
        acc = 0;
        target = T === 0 ? -1 : T;
      }
      selected = id;
      restartWalk();
    }
    /* A DELETION (tagpu_replay.c, the delete): the watched fork is left first for the nearest live timeline up its chain
       (fork_fallback: the original is always live), then the fork is gone and the rows' serial moves; the bay closes
       with its last fork, and a deleted row's part goes unless a live chain reads it (sweep) */
    function deleteFork(id) {
      if (id === selected) {
        let f = rows[id].parent;
        while (!rows[f].live) f = rows[f].parent;
        switchTo(f);
        if (failed) return;
      }
      rows[id].live = false;
      serial++;
      if (!anyFork()) lanes = 0;
      sweep();
    }
    /* THE TAKEOVER of seat h, with the picker open: the game stands at pickR - 1, paused, no seek pending (pickFill keeps
       that). The part of the next id (partOf) and the fork operation as seat h, in the walker's stead (THE LIFETIME,
       WATCH); the timeline watched stamped as left [INFERRED: the playing's stamps are written when it ends,
       tagpu_replay.c, and read only by a later eviction in the live game, which no demo reaches]; the evicted fork
       deleted, the new row selected; then LIVE MODE with seat h the human, the cursor's glide back from the row to seat
       h's cursor starting now. THE BAR AT THE TAKEOVER (MEASURED, m0t-facts.md #1f): the memo is carried, not reset --
       the watch mode's zeros, the viewer's, as the game's memo persists through a seek and a fork -- so it eases from 0
       toward seat h's stored values; the DisplayTimer is R - 1, so the step that plays R samples seat h's rates and
       raises it to R + 29. The game after the fix that runs a takeover's live game at once (takeover-pause-fix-spec.md)
       draws the takeover's own frame at R - 1 before its first batch of ticks; the wiki's first live frame is R's. */
    function takeOver(h) {
      const id = rows.length, q = partOf(id, pickR, pickParent, h);
      if (!q) return;
      if (walker) { walker.free(); walker = null; }
      bg = false;
      const before = rows.filter(r => r.id && r.live).length;
      if (!forkMember(R.game, { id, R: pickR, part: q.part, mode: q.mode, seat: h })) return;
      rows[selected].used = R.tick;
      if (pickEvict >= 0) rows[pickEvict].live = false;
      rows.push({ id, parent: pickParent, R: pickR, last: pickR - 1, live: true, used: R.tick, mode: q.mode, part: q.part, log: [], seat: h });
      selected = id;
      sweep();
      LIVE = true;
      human = h;
      rw = 'idle';
      arm = 'none';
      G = 0;
      back = clock;
      tkFull = before >= FORK_LIMIT;
      quiet = sound();
      dt = pickR - 1;
      toastOf('taking_over', pickR - 1, -1, h);
    }
    /* THE PICKER'S KEYS (tagpu_hudinput.c pick_act): true when the press is one of them. rew, fwd, play and the track
       close the picker and go on to act; Take over asks, pausing the playback, or closes; a row acts only with the picker
       open and on a seat with a unit (a dimmed row has no hot area), else it is ignored and counted */
    function pickAct(c, value) {
      if (c === 'rew' || c === 'fwd' || c === 'play' || c === 'track') { pick = 'closed'; return false; }
      if (c === 'takeover') {
        if (pick !== 'closed') pick = 'closed';
        else { paused = true; pick = 'asked'; }
        return true;
      }
      if (c !== 'takeas') return false;
      const seats = R.game.gameNow().seats;
      if (pick !== 'open' || !(Number.isInteger(value) && value >= 0 && value < seats.length) || seats[value].units === 0) ignored++;
      else { pick = 'closed'; takeOver(value); }
      return true;
    }
    /* THE LANES' KEYS (tagpu_hudinput.c lane_act): true when the press is one of them. The bay toggles the lanes while
       the file holds a fork; a lane, its Save and its Delete act on a fork the open lanes list, and Back to main
       recording while a fork is watched; anything else is ignored and counted. A lane and Back to main clear Delete's
       arm; the watched lane does nothing. Save writes a fork once; Delete arms, then deletes */
    function laneAct(c, value) {
      if (c === 'lanes') { if (anyFork()) lanes ^= 1; else ignored++; return true; }
      if (c === 'main') { if (lanes && selected !== 0) { armed = 0; switchTo(0); } else ignored++; return true; }
      if (c !== 'lane' && c !== 'export' && c !== 'delete') return false;
      if (!lanes || !liveFork(value)) { ignored++; return true; }
      if (c === 'lane') { armed = 0; if (value !== selected) switchTo(value); }
      else if (c === 'export') {
        if (saved.includes(value)) toastOf('cannot_save', value);
        else { saved.push(value); toastOf('saved', value); }
      } else if (armed !== value) { armed = value; toastOf('click_delete', value); }
      else { armed = 0; deleteFork(value); toastOf('deleted', value); }
      return true;
    }
    /* THE PICKER'S FILL (tagpu_hudinput.c pick_fill), after the game's part of each watch step, the playhead p the
       seek's target while one is pending: asked, it plans at p + 1 (the chain's last at most), once more one tick on
       when that is a segment's first, and snaps the playback back to R - 1; sought, it opens when the game stands at
       R - 1; open, it closes when the playback runs or the playhead leaves R - 1 (every wiki seek completes, and every
       press that would start another closes the picker first). A switch closes it; a deletion plans it again at R,
       which keeps it with the new eviction or closes it. */
    function pickFill() {
      const p = target >= 0 ? target : R.game.tick;
      if (pick === 'asked') {
        let q = plan(Math.min(p + 1, chainLast()));
        if (!q.ok && q.next) q = plan(q.next);
        if (!q.ok) { toastOf('cannot_take', p); pick = 'closed'; return; }
        pickR = q.R;
        pickEvict = q.evict;
        pickParent = q.parent;
        pickTl = selected;
        pickSerial = serial;
        if (!(target < 0 && R.game.tick + 1 === pickR)) seek(pickR - 1);
        if (failed) return;
        pick = 'seeking';
      }
      if (pick === 'seeking') { if (target < 0 && R.game.tick + 1 === pickR) pick = 'open'; }
      else if (pick === 'open' && (!paused || target >= 0 || R.game.tick + 1 !== pickR)) pick = 'closed';
      if (pick !== 'closed' && selected !== pickTl) pick = 'closed';
      if ((pick === 'seeking' || pick === 'open') && serial !== pickSerial) {
        pickSerial = serial;
        const q = plan(pickR);
        if (q.ok && q.R === pickR) pickEvict = q.evict;
        else { toastOf('cannot_take', pickR - 1); pick = 'closed'; }
      }
    }
    // THE ARM's hold (tagpu_hudinput.c lane_fill): only while the lanes are open and list its fork
    function laneFill() {
      if (armed && (!lanes || !liveFork(armed))) armed = 0;
    }

    /* ONE STEP, a demo tick: while the walk runs, WALK of its ticks and nothing else; then the viewer's clock (a held
       wait releases when no seek is pending and stalls past its max) and the game's part; live, the arm's fill and the
       top bar's frame after it */
    R.step = () => {
      if (done) return;
      R.tick++;
      if (walker && !bg) {
        const n = Math.min(WALK, chainLast() - walker.tick);
        for (let i = 0; i < n && !walker.done && chainStep(walker); i++) publish();
        if (!failed) { if (walker.done || walker.tick >= chainLast()) endWalk(); else walked = walker.tick; }
        done = isDone();
        return;
      }
      if (wait && target < 0) wait = null;
      else if (wait && R.tick - wait.from > wait.max) stalled = true;
      if (!wait) feed(++clock);
      if (LIVE) {
        if (!failed) livePlay();
        if (!failed) { armFill(); barFrame(); }
      } else if (!failed) {
        const ran = play();
        if (TK && !failed) { pickFill(); laneFill(); }
        if (TK && !failed) bgWalk(ran);
      }
      done = isDone();
    };
    // the game ticks the next step runs as the state stands (THE BOUND)
    R.cost = () => {
      if (done) return 0;
      if (LIVE) return rw === 'idle' ? (R.game.tick < end && !R.game.done ? 1 : 0)
        : rw === 'past' && target >= 0 && R.past ? Math.min(SEEK, target - 1 - R.past.tick) : 0;
      if (walker && !bg) return Math.min(WALK, chainLast() - walker.tick);
      const g = target >= 0 ? Math.min(SEEK, target - R.game.tick) : paused ? 0 : Math.min(Math.floor((acc + speed) / 10), chainLast() - R.game.tick);
      return walker ? g + Math.max(0, Math.min(WALK - g, chainLast() - walker.tick)) : g;
    };
    /* Try it yourself's press, acting at once: one of PRESSES, a takeover entry's watch mode also of its Take over set,
       live one of LIVE_CONTROLS (anything else does nothing); for the track the game tick under the pointer, an integer
       the seek clamps to the game (live, to the rewind's span), for a picker row its seat, for a lane's controls the
       fork's id. None reaches a replay done or still walking before it shows (a background walk is no such walk),
       which the page does not show yet. */
    R.press = (c, value) => {
      if (done || (walker && !bg) || !(LIVE ? Sim.LIVE_CONTROLS : TK ? TK_PRESSES : PRESSES).includes(c)) return;
      act(c, value);
      done = isDone();
    };
    /* TRY IT YOURSELF'S WORLD INPUT, live (rewind-spec.md 7.8): {move: [x, z]}, {down: key} or {up: key}, given between
       two steps. The world takes it only after a fork (the original is the recording), every one while the rewind is
       idle and a move alone while the strip is open (the game draws the armed building's ghost at the pointer then,
       MEASURED m0-facts.md #1), none in the past; and only while the log can hold it, keeping LOG_KEEP back while idle
       and one entry, the release's, while open (THE BOUND). LOGGED FIRST, as the human seat's input (a takeover entry's
       live mode: the seat taken over). Anything else does nothing. */
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
        cursor: t ? { from: t.from, fromValue: t.fromValue, fromWatched: t.fromWatched, to: t.to, value: t.value,
          watched: t.watched, at: t.at,
          drag: t.drag ? { drag: t.drag.drag, over: t.drag.over } : null } : null,
        held: t && clock >= t.at + HUD_DOWN && clock < release ? t.to : null,
        marks: marks.map(m => m.slice()), walked,
        // a takeover replay's toast carries the evicted fork (-1 none) whatever made it, as a live one's
        books: books.slice(), toast: toast ? (TK ? { evict: -1, ...toast } : { ...toast }) : null,
        question: question ? { wasPaused: question.wasPaused } : null, left,
      };
      /* LIVE: the rewind (rewind-spec.md 7.10): its state, the GameTime it opened at, the span, the world shown; the arm,
         its start and the fork it evicts (-1 none); the track's last seek; the clash; the entries the logs hold; the
         timelines in creation order and the selected one; the past simulation; the top bar's memo and DisplayTimer;
         the return glide's start; whether the logs hold all the wiki keeps of Try it's input; the human seat */
      if (LIVE) v.live = {
        rw, G, first: FIRST, last, now: nowTick(), arm, armR, armEvict, dragT, clash, nlogged, selected, human,
        timelines: rows.map(r => ({ id: r.id, parent: r.parent, R: r.R, last: r.last, live: r.live, used: r.used, mode: r.mode, nlog: r.log.length })),
        past: R.past ? { tick: R.past.tick, target, gen: pgen } : null,
        bar: { be, bm, rates: rates.slice(), dt }, back, full: nlogged >= LOG - LOG_KEEP,
      };
      /* TAKE OVER (a takeover entry): the mode, the human seat (-1 while watching); the picker, its plan (R, the fork it
         evicts or -1, the parent, the timeline and the serial it was made on); the lanes open, the fork armed for
         deletion (0 none), the selected timeline, its chain's last and the tick the track fills to; the rows' serial,
         the forks saved, whether the walker walks in the background and its tick (-1 none), whether the takeover's
         picker drew the fork limit's warning; the timelines in creation order */
      if (TK) v.tk = {
        mode: LIVE ? 'live' : 'watch', human, pick, pickR, pickEvict, pickParent, pickTl, pickSerial, lanes, armed, selected,
        chainLast: chainLast(), fillTo: fillTo(), serial, saved: saved.slice(), bg, walker: walker ? walker.tick : -1, tkFull,
        timelines: rows.map(r => ({ id: r.id, parent: r.parent, R: r.R, last: r.last, live: r.live, used: r.used, mode: r.mode, seat: r.seat })),
      };
      return v;
    };
    // why the replay failed, or null: a fork part the viewer's fork found wrong, a refused fork operation, a make that threw
    R.why = () => why;
    /* a live or a takeover replay's timelines as a re-simulation reads them, copies in creation order: each one's id,
       parent, R, last, mode, seat, part (the fork part's inputs, null for the original) and log; a check plays a chain
       straight from them (simcheck.mjs, the seek and now claims). [] for any other replay. */
    R.timelines = () => rows.map(r => ({ id: r.id, parent: r.parent, R: r.R, last: r.last, mode: r.mode, seat: r.seat, part: r.part,
      log: r.log.map(x => ({ ...x })) }));
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
      if (walker.done || walker.tick >= chainLast()) endWalk();
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
    CONTROLS, PRESSES, TK_PRESSES, SPEEDS, SPEED0, SEEK, WALK, STEP, NEAR, BOOKS, MARKS, FILL, HUD_DOWN, HUD_UP, DEATH, DEFEAT, TOASTS,
    GAME_END_MAX: Sim.GAME_END_MAX, REFRESH_TICKS, FORK_LIMIT, TIMELINES, refreshBin, isRefresh, forkEvict,
    RW, ARMS, FIRST, RATE, LOG, LOG_KEEP, AT_GAME, PICKS, MODES,
  };
})();
if (typeof module !== 'undefined') module.exports = WikiReplay;
