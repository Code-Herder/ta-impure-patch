/* replay.js -- the in-game wiki's replay layer: a game entry's demo (research/notes/ingame-wiki-replay-plan.md) played
   the way the replay HUD plays a recording. It holds the viewer's clock and the HUD's state, and owns the game
   simulations; sim.js knows nothing of the HUD. tagpu/ddraw/src/tagpu_wikireplay.c is its twin, function for function
   and under its names: tools/wiki/simparity.mjs compares the two demo tick by demo tick, through the state hash of
   each and the replay's own (tagpu_wikireplay.h), and tools/wiki/simcheck.mjs checks every demo's claims.

   TWO CLOCKS (decision 4). The demo's game script is played by a game simulation (WikiSim.game) on game ticks; the
   viewer's script, the demo's `inputs` (captions, the `seeked` wait and the HUD's presses, sim.js checkViewer), runs on
   demo ticks, one a step, and moves the game only through the HUD's controls: paused, a speed, a seek. Try it yourself
   (create) has no viewer's script: the player's presses come through press(), each acting at once.

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

   Runs in a page (the global WikiReplay, after sim.js) and under node (module.exports), as sim.js does. */
const WikiReplay = (() => {
  const Sim = typeof WikiSim !== 'undefined' ? WikiSim : require('./sim.js');
  /* tagpu_wikireplay.h's constants (simparity compares them): the controls a press names (sim.js's HUD_CONTROLS,
     WIKI_HUD_*), the speeds in tenths with the one a replay starts at (hud-in-game.md: 5, 10, 20, 40, 80, 1/2x to 8x),
     the game ticks a seek and the walk play in one step (SEEK, WALK), rew's and fwd's 10 s (STEP), a bookmark's reach
     of 3 s (NEAR), the bookmarks and markers held (BOOKS; MARKS: a death per kill of a commander, a defeat per seat),
     the most game ticks a page's fill lets a replay run (FILL, the presentation's), and a press's trip (HUD_DOWN,
     HUD_UP: the popup pick's without its glide back) */
  const CONTROLS = Sim.HUD_CONTROLS, HUD_DOWN = Sim.HUD_DOWN, HUD_UP = Sim.HUD_UP;
  const SPEEDS = [5, 10, 20, 40, 80], SPEED0 = 10;
  const SEEK = 256, WALK = 256, STEP = 300, NEAR = 90, BOOKS = 64, MARKS = Sim.GAME_KILLS + Sim.SEATS, FILL = 512;
  // a marker's kind, tagpu_hudlay.h's HUD_MK_DEATH and HUD_MK_DEFEAT; a toast's, WIKI_TOAST_*
  const DEATH = 0, DEFEAT = 1, TOASTS = ['added', 'removed'];

  /* `scripted`: true to play the viewer's script (play), false for Try it yourself (create). The game plays in refuse
     mode in both: Try it of a replay presses the HUD only, so its game is still the recording (decision 18). */
  function make(entry, demo, layout, ctx, scripted) {
    const R = { tick: 0, game: Sim.game(entry, demo, layout, ctx, 'refuse') };
    // the walker makes no sound: nothing of it is heard
    let walker;
    try { walker = Sim.game(entry, demo, layout, ctx == null ? ctx : { ...ctx, snd: null }, 'refuse'); }
    catch (e) { R.game.free(); throw e; }
    const end = demo.game.end;
    // the viewer's script in the order it is fed, by tick then file order; its end, never for Try it
    const script = scripted ? (Array.isArray(demo.inputs) ? demo.inputs : []).map((e, i) => [e, i])
      .sort((a, b) => a[0].at - b[0].at || a[1] - b[1]).map(p => p[0]) : [];
    const vend = scripted && demo.end != null ? demo.end : Infinity;
    let clock = 0, next = 0, wait = null, stalled = false, caption = null, failed = false, ignored = 0, done = false;
    let paused = false, speed = SPEED0, acc = 0, target = -1, gen = 1, quiet = 0;
    let books = [], toast = null, question = null;
    /* the viewer's cursor: the last trip, {from, fromValue, to, value, at, drag}, from the previous trip's control and
       its final value, the track's tick (-1 for a button), the clock tick it began and the drag's {drag, over}; `pos`
       the track's position, the last tick it sought, which the next trip starts from */
    let cursor = null, pos = -1;
    // the walk: the markers in the order made, the walker's tick, and each seat's last counts
    const marks = [];
    let walked = 0, prev = R.game.gameNow().seats.map(() => ({ lost: 0, out: false }));

    const sound = () => R.game.gameNow().sound;
    const stopped = () => R.game !== null && R.game.gameNow().stopped;
    const isDone = () => clock >= vend || stalled || failed || stopped();
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
       press but leave:no is ignored, and leave:no with none open is; each ignored press is counted. */
    function act(c, value) {
      if (failed) return;
      if (question ? c !== 'leave:no' : c === 'leave:no') { ignored++; return; }
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
      else { paused = question.wasPaused; question = null; }
    }
    /* THE TRIP's events on clock tick c: a button acts at its release; the track seeks at its press, during a drag at
       each tick i = 1..over to to + trunc((drag - to) i / over) when that differs from the last, and at the release to
       where it is. The quotient is C's integer division: |numerator| < 2^31, and a non-integer quotient lies at least
       1/over from an integer, far beyond half an ulp. */
    function trip(c) {
      const t = cursor, P = t.at + HUD_DOWN;
      if (t.to !== 'track') { if (c === t.at + HUD_UP) act(t.to); return; }
      if (c === P) act('track', t.value);
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
          cursor = { from: cursor ? cursor.to : null, fromValue: cursor ? pos : -1, to: e.hud, value: e.hud === 'track' ? e.to : -1,
            at: c, drag: 'drag' in e ? { drag: e.drag, over: e.over } : null };
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

    /* ONE STEP, a demo tick: while the walk runs, WALK of its ticks and nothing else; then the viewer's clock (a held
       wait releases when no seek is pending and stalls past its max) and the game's part */
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
      if (!failed) play();
      done = isDone();
    };
    // the game ticks the next step runs as the state stands (THE BOUND)
    R.cost = () => {
      if (done) return 0;
      if (walker) return Math.min(WALK, end - walker.tick);
      if (target >= 0) return Math.min(SEEK, target - R.game.tick);
      return paused ? 0 : Math.min(Math.floor((acc + speed) / 10), end - R.game.tick);
    };
    /* Try it yourself's press, acting at once: a control of CONTROLS (anything else does nothing), for the track the
       game tick under the pointer, an integer the seek clamps to the game. None reaches a replay done or still walking,
       which the page does not show yet. */
    R.press = (c, value) => {
      if (done || walker || !CONTROLS.includes(c)) return;
      act(c, value);
      done = isDone();
    };
    /* THE REPLAY'S VIEW (tagpu_wikireplay.h WIKIREPLAYVIEW): the demo tick, the viewer's clock and its state, the game's
       transport, the cursor and the control held down (from the press to the release), the markers and the walk, the
       bookmarks, the toast and the question. R.game.view() is the world. */
    R.view = () => {
      const g = R.game, t = cursor;
      const release = t && (t.drag ? t.at + HUD_DOWN + t.drag.over : t.at + HUD_UP);
      return {
        tick: R.tick, clock, done, waiting: wait ? 'seeked' : null, stalled, caption, failed, ignored,
        game: { tick: g ? g.tick : 0, end, playhead: target >= 0 ? target : g ? g.tick : 0, paused, speed, acc, target, gen, quiet },
        cursor: t ? { from: t.from, fromValue: t.fromValue, to: t.to, value: t.value, at: t.at,
          drag: t.drag ? { drag: t.drag.drag, over: t.drag.over } : null } : null,
        held: t && clock >= t.at + HUD_DOWN && clock < release ? t.to : null,
        marks: marks.map(m => m.slice()), walked,
        books: books.slice(), toast: toast ? { ...toast } : null,
        question: question ? { wasPaused: question.wasPaused } : null,
      };
    };
    // frees every simulation it holds; the replay is not used after
    R.free = () => {
      if (R.game) R.game.free();
      if (walker) walker.free();
      R.game = null;
      walker = null;
    };

    // the seats as the scene and the script's tick 0 leave them, a walk that has nothing to play, the clock's tick 0
    publish();
    if (walker.done) endWalk();
    feed(0);
    done = isDone();
    return R;
  }

  return {
    play: (entry, demo, layout, ctx) => make(entry, demo, layout, ctx, true),
    create: (entry, demo, layout, ctx) => make(entry, demo, layout, ctx, false),
    CONTROLS, SPEEDS, SPEED0, SEEK, WALK, STEP, NEAR, BOOKS, MARKS, FILL, HUD_DOWN, HUD_UP, DEATH, DEFEAT, TOASTS,
    GAME_END_MAX: Sim.GAME_END_MAX,
  };
})();
if (typeof module !== 'undefined') module.exports = WikiReplay;
