# The in-game wiki's replay entries (the plan)

## Status

**Designed 2026-10-30/31 in a design interview with the owner. R0 done; the first entry,
Watching, landed (`a149e0ce`); the second, Rewind a skirmish, built in the mockup and in the
DLL on `worktree-qol_shift` -- its simulation, replay and frame, Try it yourself's world input on
the live frame and its game lane (G2, below) -- and landing; Take over and Players and the camera
not built.** The owner
wants the in-game wiki to teach the replay system and the live skirmish rewind the way it teaches
the building and reclaiming features: demos that play on the stage, and Try it yourself. The stage
shows a small game with a handful of units in a whole game frame -- the top bar, the left sidebar,
the map and the replay HUD -- and the demos operate the replay HUD's controls on it, forks
included, the rewind button at the top right among them. What is built is described as it stands
in [the in-game wiki](ingame-wiki.md), *The game frame on the stage: the replay entries*, *A game*
and *The data format*; the engine facts it rests on are in the engine map's entries of the
functions it imitates. This note keeps the decisions, the landings and what they measured.

The decisions below are the owner's, each with its reason. The existing wiki is
[the in-game wiki](ingame-wiki.md); the replay HUD is [the replay HUD in game](hud-in-game.md);
the rules the wiki copies are the recorder's ([S4b, the takeover](tadr-port/demo-recorder-s4-takeover.md),
[S4c, the fork rules](tadr-port/demo-recorder-s4-forks.md), [S4d, live rewind](tadr-port/demo-recorder-s4-rewind.md)).

## The decisions

| # | Question | Decision | Why |
|---|---|---|---|
| 1 | Where the replay's content comes from | **The wiki's own simulation** (`sim.js`, `tagpu_wikisim.c`). The recording is the demo's scene and game inputs; a seek plays them again to the target; a fork plays to T and goes on with other inputs | The wiki's guarantees carry over -- a demo shows only what the rules do, the two simulations held equal tick for tick, every mod's casting -- and only this makes Play from here real in Try it yourself. A real game's tracks turned into data were declined: forks would be canned, the stage would need projectiles, and the casting would be lost |
| 2 | Deaths | **Scripted kills**, `{ "at": t, "kill": "<unit>" }` in the game script. The unit dies at that game tick: removed, its orders dropped, a building it was making left as it is. The HUD's markers are read from the simulation's deaths and defeats, never from the script | A kill at a fixed tick is an input like any other: reproducible, replayed by every seek, covered by the parity hash. No combat in the simulation: weapons, damage, projectiles and aim scripts would be a program of their own |
| 3 | What a death looks like | **The cast's `corpse=` feature** replaces the unit at once, drawn by the reclaim entry's wreck path, and the death sound plays. No explosion art | Reuses what exists; the wreck before and after the kill makes a seek legible. The `ExplodeAs` GAF animation is a later polish |
| 4 | Timelines | **Two clocks.** The **game script** (`"game": { "inputs": [...] }`) runs on game time; the **viewer's script** (`"inputs"`, the HUD's clicks, the captions, the waits) runs on demo time and drives the game clock through the HUD: paused, a speed, a seek. In Rewind a skirmish the viewer is seat 0: before the rewind its inputs go into the game live; after Play from here they are the fork's game inputs. Take over is the same from the takeover tick. Try it yourself is the viewer, HUD only, until a fork or a takeover | One timeline cannot express a seek: every later game input would land at the wrong game time |
| 5 | Seats | **Every seat acts through the cursor-level inputs** (`"seat": n` on a game input, 0 when absent), each with its own cursor, held keys, selection and armed mode, played by the same rules. **At most 4 seats** in the data, bounded in both simulations; the demos are authored with 2, the player and an opponent, each on their side's cast | The overlays show real recorded cursors, clicks and selections, and Take over hands the viewer a seat that acted the way they will. Seat 0 in a watching demo is already a recorded seat, so the input state comes apart from the viewer in any case |
| 6 | Room on screen | **A theatre layout** for an entry that declares `"frame": "game"`: the stage spans the window under the title bar, the demos become a row of numbered tabs above the caption strip, the text a panel an *About* control opens over the stage, the tree a drawer on its own control. The frame is laid out at the stage's own pixel size | On the normal page the stage is about 560 × 370 at 1024 × 768 (measured against `art/wiki/layout.json`): the whole frame at half scale, the HUD's labels about 5 px. A second page layout in `tagpu_wikilay.c` and `wiki.js`, held equal by `wikilaycheck.py` |
| 7 | The top bar and its numbers | **The mod's own resource bar, its numbers from a non-limiting economy** per seat: storage at the start from the scene, income from extractors and generators, spending at the rate builds draw. It never slows a build, so every existing demo plays as now; **both simulations refuse a demo whose storage would fall below zero**, so a demo that passes would never have stalled in the game. The bar's art is the mod's own (decision 20). The rewind button and the sprocket are drawn over it as in the game | Fixed numbers would show metal that never moves while extractors are built; a bar without readouts is not the whole game area. The players column (`HUDSEAT`) reads the same numbers |
| 8 | The other seats after a fork | **They play on from their recorded inputs**, through the same rules on a world that may now differ; a fork's own script may give any seat inputs from T. **A fork drops every recorded kill after T**: a death in a fork comes from the fork's script alone. The text says in one line that in the game the other players become AIs | The game seats an AI for each other player (S4b); the wiki has none and grows none. Idle seats would teach that a fork freezes the opponent. Dropping the kills makes "fork before the commander dies and it lives" hold by construction, as in the game, where a death after T is not fated |
| 9 | The fork rules | **The recorder's own, from its own constants**: forks only on the refresh grid of `REC_REFRESH_TICKS` (iterations 1, 2, then one past each multiple; `inc/tagpu_recrules.h`, included, not copied) with the HUD's two-click snap back; at most three live forks; the least recently used evicted, never the original or the one watched, with the HUD's warning; forks of forks with parents. "Save as a replay" shows its toast and keeps the fork in a list of the demo's own | A player meets on the stage exactly what the game does, and a change to the recorder's interval or cap reaches the wiki. The eviction order is one pure function both simulations share |
| 10 | The entries | Four, under a new top-level category **Replays** (below). **Chat is left out** -- it would need recorded chat lines, a new input kind -- with one line of text | |
| 11 | Seeking | **Re-simulation, spread over fills**: a seek plays from tick 0 (or a later start point, if one is added) to the target at most a fixed budget of ticks a fill; the stage holds the last world drawn and the HUD shows the playhead at the target. The first C landing measures a tick's cost. **Snapshots at each refresh only if** a seek to the end of the longest demo takes more than about a second, and then held to the re-simulated world by the state hash at every refresh in the parity check | Safe by construction: the world after a seek is the world after playing to that tick, the hash `simparity.mjs` already checks. A deep copy of `WIKISIM` (a dozen heap blocks and the COB VM's state) is the riskiest code here and is not built before it is needed. A seek in one fill would freeze the frame, unacceptable in multiplayer, where the wiki is open over a running game. 8× needs 8 game ticks a fill at 30 fps, the existing bound |
| 12 | The order | **Vertical slices, one entry at a time**, each its mockup, then its C, then the game (below) | The mockup is the specification: each entry is corrected in the browser before any C is written for it |
| 13 | The top bar while watching | **The game's: the viewed record's.** At full map that is the viewer's own record, whose storages the player init zeroes, so the bar reads 0 with both fills skipped; under Sight it is that seat's numbers in its side's art. The players column carries every seat's numbers | A replay shows what the game showed. If the zeros are wrong, the fix is the game's, and the wiki follows it |
| 14 | The bottom bar | **Drawn**: PANELBOT of the viewed side, tiled from x 129 along the screen's last 32 rows, as `0x46A860` draws it | The HUD's dock sits on it in the game; the same art and loader as the top bar |
| 15 | Sight on the other side | **Today's mixed frame**: the panel and PANELSIDE stay the loaded side's, the two bars take the viewed side's. When the mid-level re-skin (OQ-B, [the takeover](tadr-port/demo-recorder-s4-takeover.md), *The mixed-side interface*) lands in the game, the wiki follows it in the same landing | The stage shows what the game does now |
| 16 | When the other seats get cursor inputs | **Deferred to Take over (T1) or the overlays (P1)**, whichever first has a demo that gives a seat other than 0 an input; until then both readers refuse a `seat` other than 0, and the opponent acts through its scene's orders | None of Watching's demos gives the opponent an input, so the parity check could not test the state a seat's input changes |
| 17 | A click on a marker | **The timeline pressed at the marker's time**, a seek there; a marker has no hot area of its own in the game | What the game does |
| 18 | Running dry | **An authored demo never runs dry**: one whose storage would fall below zero stops, `starved`, and `simcheck.mjs` fails the landing in any context where one does, so no demo's timing is ever the economy's. **In Try it yourself the builds stall at zero** as the engine's do, slowed to what the income meets | Refusing at load is impossible -- only playing it says. Clamping in Try it would show the player something the game does not do |
| 19 | The opponent's side | **The first side of the running mod's layout that is not the player's**; with one side, the opponent keeps the entry's own types | A rule that names no mod |
| 20 | The bars' art | **The mod's own, from the first landing**: the top and bottom bars composed from the side's PANELTOP, PANELBOT, badge, rects, colours and font, as R0 mapped them; the separate landing B and its whitebox are dropped | R0's reference renderer matches the game's capture, so a whitebox phase would only be thrown away |
| 21 | The stage HUD's scale | **100 %**, laid out at its default scale inside the frame | Every player sees the same demo, and the checks compare one layout |
| 22 | The page's own controls | **Kept** beside the HUD's: the page's play, replay and speed drive the demo, the HUD's the replay inside it | The page stays the same on every entry |
| 23 | Leave, then Yes, in Try it yourself | **The stage's replay ends**, with a caption saying that in the game this returns to the Replays screen | |
| 24 | The theatre's arrangement, Leave's question, the bottom bar, the viewer's cursor | Taken as the presentation spec recommends and corrected in the browser at W1: a row of tabs, the transport, Try it and About, the About panel over the stage's right, the tree a drawer, no keys strip; Leave's question at YESORNO's own rectangles, whitebox until a real Watch is measured; the bottom bar as the bare strip (a hovered unit's readout is a gap); the viewer's cursor the game's arrow at the player's cursor scale | |
| 25 | What a fork and Back to now leave of the player's input (2026-11-01) | **The game's.** Held keys, buttons and gestures are let go without acting; the selection and the armed building go too, because the game's rebuild clears them (MEASURED, *What the game's live rewind leaves*, below) | A release that acted would finish a box select or a placement the player never made; keeping the selection would show what the game does not |
| 26 | How long a fork plays in Try it yourself (2026-11-01) | **To the demo's `game.end`**, like every timeline; to play more, the player rewinds and plays from an earlier point | It adds no rule, and every seek stays within the authored game's length |
| 27 | The recorded seat 0's cursor in Try it yourself before the first fork (2026-11-01) | **Not drawn**: the player's pointer is the screen's one cursor. The world still shows what seat 0 does -- its selections, sites and buildings. In the authored demos the simulation's cursor is drawn, the viewer and seat 0 being one scripted hand | The game shows one cursor; two arrows would leave the player unsure which one is theirs |

## The entries

**1. Watching a replay** -- REPLAYS on the main menu and the browser in the text; on the stage:

1. Play and pause; the clock and its length
2. Speeds, ½× to 8×
3. Back and forward 10 s
4. Seek on the timeline: a press, then a drag
5. Markers: a commander's death and a defeat on the track; the track pressed at one seeks there
6. Bookmarks: one added at now, and the one within 3 s removed
7. Leave: the question

**2. Players and the camera**

1. Jump: the camera onto a player's view, once
2. Follow: the camera stays with them
3. Sight: their fog and line of sight
4. The overlays: Cursors, Clicks and Selections, each switched on and off
5. The stats window: opened, its measure changed, split by teams

**3. Take over a replay**

1. Take over: the picker, a seat chosen, and the viewer plays it from the snap-back point
2. The Forks bay and the lanes: the original watched, then the fork
3. A fork saved as a replay, and one deleted
4. The fourth fork: the warning, and the eviction

**4. Rewind a skirmish**

1. The button: the strip opened, the past held paused, Back to now
2. Back in steps, and a seek in the past
3. Play from here: the snap back, the second click, the fork
4. Undo a death: a fork before the commander dies, and it lives
5. Forks of forks: a rewind inside a fork

## The data

A replay entry is format 2 with these additions; both simulations refuse an entry that breaks
them, as they refuse the existing rules ([the data format](ingame-wiki.md#the-data-format)).

- **`"frame": "game"`** on the entry: the theatre layout and the whole game frame on the stage.
- **A demo's `game`**: `{ "inputs": [...] }`, the recording's script on game time, its inputs the
  existing kinds with an optional `"seat"` (0..3 in the design; 0 only until decision 16's entry) and the new `{ "kill": "<unit>" }`. A unit's
  `owner` in a scene is its seat.
- **A demo's `inputs`** on such an entry are the viewer's, on demo time: the captions, the waits
  and `{ "hud": "<control>", ... }`, each played as the cursor's trip to the drawn control, as a
  build is. The controls are the replay HUD's (play, speed, back, forward, seek with `"to"`,
  bookmark, leave, sight, follow, jump, the overlays, stats, take over and its picker's row,
  the forks bay, a lane's watch, save and delete) and the top bar's rewind button and the strip's
  (back, forward, the track, Back to now, Play from here).
- **A fork's part**: the game inputs the viewer's fork or takeover goes on with, keyed by the
  fork it belongs to.
- **A scene's economy**: each seat's storage at the start; income and spending follow from the
  casting's figures.

The keys as built, with every rule both readers refuse, are in [the in-game wiki](ingame-wiki.md),
*The data format*: `"seats"` on a scene, `"game": {end, inputs, sidebar}` on a demo, `"seat"` (0
only, decision 16) and `{ "kill": "<id>" }` on a game input, `"rival"` and `"corpse"` on a unit,
the economy's figures, the viewer's `{ "hud": … }` presses and `{ "until": "seeked" }` wait, and
for Rewind `"live": true` on the entry and a demo's `"forks"`, its fork parts. Watching's demos
press the replay HUD's `play`, `speed`, `rew`, `fwd`, `track` (with `to`, and `drag` with `over`),
`bookmark`, `leave` and `leave:no`; Rewind's the live HUD's `rewind`, `rew`, `fwd`, `track`,
`live_play` and `live_now`, each entry the one set only. The other controls above wait for their
entries.

## The landings

| # | Landing | Carries | Status |
|---|---|---|---|
| R0 | Research, no code | the resource bar's art and drawing; the side panel while watching; the HUD's inputs; a wiki tick's cost | **done 2026-10-31** (*What R0 found*, below) |
| W | **Watching a replay**: the simulation and the data (C1-C5), the mockup's presentation (F1-F4) and the DLL's (G1-G5), as one landing | below | **landed 2026-10-31** (`a149e0ce`) |
| R | **Rewind a skirmish**: the recorder's rules shared (C1, its own landing), then the simulation, the data, the replay's live mode and the entry (C2-C5), the mockup's frame and the DLL's (F1, G1), the release rule, the threads and Try it's world input in the DLL (G2), and the docs, as one landing | below | **C1 landed** (`8005c54f`, `6c976067`); **C2-C5, F1, G1, the release rule and G2 built** on `worktree-qol_shift`, the landing in progress |
| T1-T3 | Take over a replay | the picker, the Forks bay, the lanes, the eviction; a second seat's cursor (decision 16) | not started |
| P1-P3 | Players and the camera | Jump, Follow, Sight (a fog per seat), the overlays, the stats window on the stage (its own chart image) | not started |

Watching went first because it builds the frame, the two clocks, seek and the kills, which every
other entry stands on, without forks. **Rewind is second**, the owner's own example, whose fork
machinery Take over reuses; then Take over, then Players and the camera.

**Watching, as built.** The planned W1 (mockup), W2 (C) and W3 (in the game) became one landing
cut along the two specs' seams: the simulation and the data in five steps, each in `sim.js` and
its C twin at once, and the presentation in four mockup steps and five DLL steps. The planned
landing B (a whitebox top bar) was dropped by decision 20.

| step | what it carries | commits |
|---|---|---|
| C1 | the castings: the `commander` cast (the tenth), every cast's economy figures and its corpse, in `gfx.py`, `tagpu_wikiasset.c`, `sim.js`, `text.js` and `tagpu_wikidata.c` | `6125ce11` |
| C2 | game mode in both simulations: seats, `rival`, kills (the builder's running order dropped with cant), wrecks by the engine's placement rules, the death sound and its asset (ExplodeAs → `soundhit`), the per-unit ledgers and the economy pass in `refuse` and `stall`, hash item 12; `watching.json` | `19f63da5`, `0d9c8d3e` (the death sound's weapon table is the build's weapon array) |
| C3 | the replay layer, `replay.js` and `tagpu_wikireplay.c`: the two clocks, the controls, the walker and its markers, seek by re-simulation; the viewer's data rules; the seven demos | `6861004f` |
| C4 | the tree's Replays category naming Watching; the in-game check playing a game entry's demos and logging each replay's cost | `a4c6aef3` |
| C5 | SEEK, WALK and FILL set to 64 from the measured cost (*The measured costs*) | in `6276cf23` |
| F1 | `bar.js`, the side pack's bar art, logos and GUI table, `barcheck.mjs`'s JS half | `a9543870` |
| F2 | the theatre in the mockup: About, the Entries drawer, the numbered tabs, `layout.json` `theatre` | `74d0060a` |
| F3 | `frame.js` and the stage's frame mode: the watcher's view, both bars, the emblems, the replay HUD at 100 %, Leave's question, the viewer's cursor, pacing by cost, the held view, Try it's presses, `leave:yes` | `7a632e0f` |
| F4 | `framecheck.mjs`'s JS half | with G3, `e80fea6b` |
| G1 | the bars' art in the DLL's sidebar loader, the three exe names, `tagpu_wikibar.c`, `barcheck.mjs`'s C half | `28b6c9c1` |
| G2 | the theatre in `tagpu_wikilay.c`, its check states, `WIKI_THEATRE_TABS` in both readers | `7d6a78c6` |
| G3 | `tagpu_wikiframe.c`, the stage's frame mode in C, `HOP_BAR` and the bar texture, `tagpu_huddraw_partition`, `framecheck.mjs`; the game entry's 4:3 bound (Watching 64 cells wide); `wikilaycheck.py`'s theatre on Watching and its stage HUD pairing | `e80fea6b`, `8c0cb243`, `614752a4` |
| G4 | the threads: the record and the theatre's fields in the packet, the replay in the game thread's fill paced by cost, the render thread's own stage HUD and bars, the hit map's sources, the HUD pass's seven lists; the `watch` lane and `mods`' w6 | `6276cf23`, `f1b6d74a`, `4d9ca0d4` |
| G5 | the documentation pass: the engine map (the economy, the wrecks, the death sound, MobileBuild's result 8, the resource and bottom bars, with the corrections R0 and the scouts found), the module note, [the in-game wiki](ingame-wiki.md) and this note | the `docs:` commit |

**Not closed by Watching** (each also in [the in-game wiki](ingame-wiki.md), *Gaps*): the other
three entries; a second seat's cursor (decision 16); Leave's question and the watcher's column are
not yet compared with a real Watch's (the question's place and the column's state are
[INFERRED]); a hovered unit's readout on the bottom bar (decision 24); sound at 2× to 8× (*Open*);
Escalation's dear ticks (*The measured costs*).

**Rewind, as built.** One landing again, cut along the spec's seams, the recorder's part ahead of
it on its own.

| step | what it carries | commits |
|---|---|---|
| C1 | the recorder's rules in one pure header, `inc/tagpu_recrules.h` -- `REC_REFRESH_TICKS`, `REC_FORK_LIMIT`, `RECR_TIMELINES`, `reclossy_is_refresh` and the eviction `rec_fork_evict`, moved out of the recorder -- and `replay.js`'s twins, checked against it | `8005c54f`, `6c976067` (on `main`) |
| C2 | the simulation's fork (`tagpu_wikisim_fork`, `sim.fork`) and release (`tagpu_wikisim_release`, `sim.release`); the column's row 0 at world z −32 in a game with the sidebar on (DZ); `fed`, the seat-0 inputs fed | `71bde6a3` |
| C3 | the data: `live`, the live HUD's three controls, a demo's `forks` | `cde880e8` |
| C4 | the replay's live mode (`replay.js`, `tagpu_wikireplay.c`): the live and past simulations, the chain's re-simulation, Play from here's arm, the forks and their eviction by the recorder's own functions, Back to now's release logged on the timeline, the top bar's memo; `simparity.mjs`'s live pass and LIVE_TRACE | `ce60384d` |
| C5 | `art/wiki/entries/rewind.json`, its five demos, and `simcheck.mjs`'s live claims; the scene's stage description (`art/wiki/stage/rewind.field.json`, by `stagemap.py build` from the field scene's recipe) and the text's sentence on where every timeline stops (decision 26) | `c66f3be4`, `7b7b7fa4` |
| F1, G1 | the live frame in the mockup and the DLL: the views by mode, the record's live fields, the live HUD on the stage, the wiki emblem at `right[0]`, the top bar's numbers, the one cursor | `10f44a02` |
| the release rule | the release drops the trip it cuts off; the live data rule; `simcheck.mjs`'s release sweep and `wikitest sweep` | `2383ed38` |
| G2 | the DLL's Try it world input on the live frame (`tagpu_wiki.c` `live_world`, the topmost hit; the held move `live_flush`; every input through `tagpu_wikireplay_input`) and the one-cursor answer `TAGPU_PK_WIKI.try_world`; `TAGPU_CMD_WIKI`'s drawn span (`hud_first`, `hud_G`); the dump's live fields (`live_json`); `wiki_regression.py`'s `rewind` lane (r1)-(r5) and the `mods` lane's (w7), both in the gate; `simparity.mjs --frame`'s world shown and `pending`, `barcheck.mjs --compose`'s NUM | `c74198fe`, `f1cc29ed` |
| D | the documentation pass | the `docs:` commit |

**Decisions taken in the build**, each held by both simulations and checked by `simcheck.mjs`:

- **The game's end is 2700 ticks** (1:30) in every demo, long enough for the stall case to run
  dry in every pack with the units' scripts playing: a builder starts work only once its script's
  INBUILDSTANCE gate opens, and TA Twilight's game runs dry only by tick 2250 (MEASURED 2026-11-01;
  1800 left it unstarved). Every timeline holds at it, a Try-it fork's included (decision 26), and a
  seek to it takes 43 steps of `WIKI_REPLAY_SEEK` (64).
- **Seat 0 builds through the ARMCK.** The fixture layout (`art/wiki/side/fixture.json`), the
  sidebar the offline checks play, pages a build menu on the ARM side only for ARMCK and ARMACK,
  so the commander has none there: in the four demos with the sidebar the opening's ARMSOLAR and
  ARMMEX are the ARMCK's, each the cursor's trip to its button, and the commander assists the
  solar. With every seat's storage cut to 200 (the stall case) the game then runs dry in every
  context simcheck plays (MEASURED 2026-11-01 without the units' scripts: refuse mode starved at
  tick 120, 150 or 240). *Undo a death* plays without the sidebar, where a build arms at once: the
  commander builds the solar and the ARMCK the extractor, each build whole on one tick (select,
  arm, place).
- **The release drops the trip in flight.** Back to now can land while seat 0's script is inside
  a build trip; the release empties the selection the trip was planned from, so the rest of the
  trip would press the MAIN2 the column now shows and lock the game. The release therefore drops
  every generated input not yet fed (`trip_next = ntrip_in`, `genNext = gen.length`) with the
  held state, and the fork drops a timeline's trips only by calling it, so one place does it. A
  release costs the game at most the one order in hand: a trip it cuts off, or a building armed
  and not yet placed.
- **A live build selects its builder on its own tick** (the live data rule, a load refusal:
  `tagpu_wikidata.c` `live_builds`, `sim.js` `checkLiveBuilds`). In a live entry each build
  naming a type, in seat 0's game script and in every fork part, has on its own tick and before
  it the click that selects its builder: a move, then select pressed, then select released. It
  holds **by construction**: the release runs only between two ticks (Back to now and the fork
  call it between steps, and a re-simulation applies a logged release at its tick before the
  step), so it cannot fall between that click and the build. Reselecting before each build on an
  earlier tick did not close it: a release could still land between the select and the build.
  The refusal names the demo, the fork part and the tick.
- **The sweep proves it.** `simcheck.mjs`'s release sweep plays each live entry's distinct games
  with the release after every tick from 0 to the script's last input, each to its end, and fails
  on a lock, a starved pass, a kill that found no unit or a stalled wait: 2266 releases over
  Rewind's four distinct games (after ticks 0..242, 0..410, 0..710 and 0..900). It runs without
  the units' scripts, which make a game about 75 times dearer (MEASURED 2026-11-01: 0.75 s against
  10 ms for 2700 ticks); a starve that only the scripts' later start of work would bring is the
  one case it does not play. `simparity.mjs` holds the C sim to `sim.js` at every release's end
  through `wikitest sweep`, in two contexts.

**Not closed by Rewind** (each also in [the in-game wiki](ingame-wiki.md), *Gaps*): the game
lane plays Rewind on the base game only -- under the five mods of the compatibility suite the
`mods` lane holds its live bars (w7) and nothing else of the entry; the page's cost on
the game thread is over the 0.5 ms target under the gate's load (*The measured costs*), and no lane
fails on it; the eviction is checked by LIVE_TRACE and shown by no
demo (Take over's demo 4 shows it); the arrow and diamond the game draws round the recorded
commander in the past are the overlays', P1's [INFERRED]; the live game has no speed of its own
(one game tick a demo step, the game's 1×), and the Pause key during a rewind is not modelled.

## The measured costs

MEASURED 2026-10-31 on the reference setup, the DLL's i686 build under Wine; the tick costs by
`tagpu_wiki.check` under every setup of the compatibility suite, the rest by
`wiki_regression.py`'s `watch` and `demos` lanes at 1920 × 1080.

- **A replay tick** costs 1.5 to 3.8 µs on average on every setup but Escalation, where it costs
  11.6 to 15.3 µs. The dearest 64-tick step is 3.57 ms under Escalation and 0.41 ms or less
  elsewhere.
- **Why Escalation's are dear**: its commanders' scripts (ARMCOM, CORCOM) scan every unit id from 1
  to `MAX_ID` -- 1024 in the wiki's host, `WIKICOB_UNITS` -- for its height, team, build left and
  alliance every 2.5 to 5 s, so about one tick in 55 costs 0.7 to 1.4 ms natively (gcc `-O2`,
  Watching's demos). It is the mod's content, not a defect of the host. Answering `MAX_ID` with the
  highest id the simulation has spawned would cut the scan to the units that exist; whether the
  host should is **open, the owner's to decide** (it changes what a script reads, so both
  simulations and the parity check would follow it).
- **The pacing** (C5): SEEK, WALK and FILL at 64 game ticks hold Escalation's worst fill at
  1.75 ms natively (4.1 ms at 256); a seek to the end of Watching's game (1800 ticks) takes 29
  steps, about a second at 1×, and one to `WIKI_GAME_END_MAX` (5400) 85, under three seconds.
- **The page**: Watching's page cost the game thread at most 0.462 ms in one frame over 174
  frames. The `demos` lane's (d5) maximum over every page demo is 0.628 ms, equal on `main` and on
  this branch, so it is not this landing's; it is over the plan's 0.5 ms target, and the lane does
  not fail on it.
- **The render thread**: the stage HUD's two builds at most 0.159 ms in a frame; the bars 0.301 ms
  in a frame that composes them, nothing in one that keeps them; the wiki's layer peaked at 1 309
  of 32 768 instances (87 batches of 1024).
- **Rewind in the game** (MEASURED 2026-11-01, `wiki_regression.py`'s `rewind` lane at
  1920 × 1080, first as the only lane, then in the full gate at `-j 6`): the rewind button's hit
  within a pixel of frame pixels Wg − 75 .. Wg − 48, rows 2..29 (r1); demos `steps` and `chain`
  paused at nine ticks each, the record and the replay's hash equal `replay.js`'s (r2); at the open
  strip, the armed second click, the fork's first ticks and `button` at tick 90, the column and both
  bars pixel-equal to the mockup's, the emblems' columns and the HUD's controls left out (r3); in
  Try it an ARMSOLAR placed by world input stands, Back to now from the open strip closes it with
  the selection and the armed building kept, and from the past it releases both, logged on F1,
  the fork itself taking 0.019 to 0.029 ms (r4). The longest seek, a past game re-simulated from
  tick 0 to the strip's end (r5): 75 demo steps and 2.5 s alone, 87 and 3.3 s in the gate. The page
  cost the game thread at most 0.413 ms in one frame over 503 frames alone, and 0.558 ms over 502 in
  the gate -- over the 0.5 ms target, which the lane prints and does not fail on. The render
  thread's stage HUD at most 0.029 ms in a frame and the bars 0.201 ms alone (0.038 and 0.268 ms in
  the gate). The `mods` lane's (w7) held the live frame's two bars, `bar.js` given the record's
  numbers, pixel-equal on all six setups. The gate's 38 lanes (22 offline checks, 10 modes, 6
  setups): 37 passed; the page lane's fold check (p3) did not count Rewind's row under Replays,
  and with it counted (`f1cc29ed`) the page lane passed on its own.
- **The packet**: `TAGPU_PK_WIKI` 167 616 bytes, the frame record 1 592 of them with Rewind's live
  fields, the head 328 with the replay's serial; `try_world` sits in padding, so the area did not
  grow with it. `TAGPU_CMD_WIKI` 10 040, the live strip's drawn span (`hud_first`, `hud_G`)
  included (sizeof on the DLL's compiler, 2026-11-01).

## What R0 found

- **The resource bar** is drawn by DrawGameScreen (`0x468E3F..0x46960B`) from art read once a
  process by `0x491200`: frame 0 of `PANELTOP` and `PANELBOT` from `anims\<intgaf>.GAF` (the
  side's `intgaf` in `gamedata\sidedata.TDF`, loaded by `0x429870`), the side's rects, fill colours
  and `fonts\<font>.FNT` from `sidedata.TDF` (`0x431A60`), the colour badge from `logos.GAF`
  `32xlogos` (`0x4318C0`). PANELTOP is laid once at x 129 and PANELBOT repeated to the screen's
  edge; then the badge, the two fills with their storage ticks, and the readouts. The stored values
  ease an eighth of the way a frame and the rates are sampled once a game second. A reproduction in
  Python matched a capture of our renderer on every pixel but Impure's sprocket (36 464 of 36 832,
  MEASURED 2026-10-31); Total Mayhem, Twilight, Escalation and TA Zero each differ in art or rects.
  The facts are in the engine map, re-checked against the disassembly: [ui.md](engine/ui.md),
  *The resource bar tiles in a loop*, and [render.md](engine/render.md), *The four rects*.
- **The side panel while watching** is `<prefix>MAIN2` of the side the level loaded, the
  recording's controlled seat's at full map; Sight does not change it. The viewer controls the
  spectator record, whose unit list is empty, so nothing is selected and no build menu shows
  [INFERRED from the code: no capture of a Watch's panel exists].
- **The HUD's inputs**: `tagpu_hudlay_build` needs no change; the stage needs a second `HUDOPS`
  drawn with the HUD's art and its own hit map. The markers and the stats cover the whole recording,
  ahead of the playhead, so a replay demo plays its game script to the end once before it shows.
  `hud.js` runs only in a page. Settled in W1: a press's cursor trip is the replay's state
  (its hash covers the viewer's cursor), not the game simulation's, and the stage builds its own
  `HUDOPS` on the render thread.
- **A tick's cost**: every demo, with the units' scripts, natively at `-O2`: the longest 1 068 ticks
  in 3.56 ms, the dearest 1 033 ticks in 4.37 ms (4.78 ms with the DLL's compiler under Wine), the
  worst single tick 33 µs (MEASURED 2026-10-31). A seek needs no snapshots. A seek must count the
  sound events it re-simulated as heard, or they play at once.

## What the game's live rewind leaves

MEASURED 2026-11-01 before Rewind's code was written, on the reference setup under Wine: `main`
`a149e0ce`'s DLL, one four-seat skirmish (the `battle` fixture's load), true sight, driven with
`tacli rewind`, `shot`, `peek` and `ui`, the commander selected (unit `+0x110` bit 4) and a
building armed from its build page (`main+0x2CC3` 14) before each open. The engine-side facts are
in the engine map ([recorder.md](engine/recorder.md), *What the live rewind leaves of the
player's screen and input*; [input.md](engine/input.md), the order byte's cancels).

- **Open** keeps everything: the column (the build page), the selection, the armed mode, and the
  armed building's ghost drawn at the pointer over the world. The wiki's `player` view.
- **The past** (a seek to 700 showing 699): the player controls the spectator record 10 and views
  their own (`main+0x2A42` 10, `+0x2A43` 0); the column shown at the open stays (not idle MAIN2);
  nothing selected, nothing armed (`0x2CC3` 1, `+0x2CC4` keeping a stale type); the fog the seat's
  own at the GameTime shown; the top bar's stored values the past's, eased, and its rates the ones
  sampled before the seek (+25/−0 energy shown at 699 while the record held 50.15 of expense).
  The wiki's `past` view.
- **Back to now** (at 2501, 165 ms) and **a fork** (asked at 1500, taken over at 1201, 221 ms)
  restore neither the selection nor the armed mode, and show the side's MAIN2. The wiki's release.
- **The resource bar's DisplayTimer** (the viewed record's `+0xF8`) is 0 in a record no frame has
  drawn, 30 at GameTime 1, 60 from 31, 90 from 61; a seek and a fork set it to the GameTime shown
  (399 after a seek to 400, 1200 at the fork). After Back to now at 2501 it read 2561 at 2536: set
  to G and raised at 2502 and 2532 [INFERRED from the arithmetic].
- **The memo** of the bar persists through a seek and a fork (the seek's own frame showed
  9748.0 / 9697.0 while the stored values were already 10 000), and at GameTime 1 it equalled the
  stored values (10 000).

Not measured: the open and the past with the pointer on the strip; the memo frame by frame after
a seek (the reads were 15 frames apart and it had converged); what keys or buttons the player holds
through a fork or Back to now; the arrow and diamond the game draws round the recorded commander in
the past, [INFERRED] the replay overlays.

## Open

- **Sound at speed**: a re-simulated tick plays nothing (a seek's end marks every sound it
  re-simulated as heard); what the stage plays at 2× to 8× should follow what the game does, which
  is not measured [INFERRED until then].
- **Escalation's `MAX_ID` scan** (*The measured costs*): whether the wiki's host answers `MAX_ID`
  with the highest id spawned is the owner's decision.
- **Chat** and **combat** are out of this program. The `ExplodeAs` explosion is a later polish.
