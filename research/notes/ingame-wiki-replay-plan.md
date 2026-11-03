# The in-game wiki's replay entries (the plan)

## Status

**Designed 2026-10-30/31 in a design interview with the owner. R0 done; the first entry,
Watching, landed (`a149e0ce`); the second, Rewind a skirmish, landed (`078ac2cb`) -- its
simulation, replay and frame, Try it yourself's world input on the live frame and its game lane;
the game's own takeover fix it needed, P0, landed (`fa62ae5e`); the third, Take over a replay,
landed (`e96fa13f`) -- a cursor for every seat, the picker, the Forks bay and its lanes, the takeover into a live
game as any seat, Try it in the game and its game lane; the fourth, Players and the camera,
landed (`b0d8b276`)** (*Players, as built*, below). The owner
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
| 14 | The bottom bar | **Drawn**: PANELBOT of the loaded side, tiled from x 129 along the screen's last 32 rows, as `0x46A860` draws it. After a takeover of a seat of the other side the game draws the bottom strip the load's (MEASURED 2026-11-01, M0-T: a CORE seat taken on an ARM load, the strip pixel-identical to the ARM takeover's, 0 of 27 776 pixels; [engine/ui.md](engine/ui.md), *The resource bar tiles in a loop*), so the wiki draws it from the load's side in every frame | The HUD's dock sits on it in the game; the same art and loader as the top bar |
| 15 | Sight on the other side | **Today's mixed frame**: the panel, PANELSIDE and the bottom bar stay the loaded side's, the top bar takes the viewed side's -- as the game draws a takeover of the other side (MEASURED 2026-11-01, M0-T: the top bar's background, labels and badge CORE's after a CORE seat is taken on an ARM load, 8 319, 1 428 / 1 552 and 800 pixels apart from the ARM takeover's; the column, minimap and bottom strip 0 pixels apart). In the wiki the top bar's side is the record's `top_side`. When the mid-level re-skin (OQ-B, [the takeover](tadr-port/demo-recorder-s4-takeover.md), *The mixed-side interface*) lands in the game, the wiki follows it in the same landing | The stage shows what the game does now |
| 16 | When the other seats get cursor inputs | **Settled by Take over**: every seat of a game has its own cursor, held keys and buttons, selection and armed mode in both simulations, and a game input names its seat (*Take over, as built*, below). A seat other than 0 is admitted only in a game whose sidebar is false, since the column is seat 0's | Take over's first demo has the opponent, Bram, act through cursor inputs, and Try it may take over any seat |
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
  existing kinds with an optional `"seat"` (one of the scene's, 0 when absent; a seat other than 0
  only in a game whose sidebar is false, decision 16) and the new `{ "kill": "<unit>" }`. A unit's
  `owner` in a scene is its seat.
- **A demo's `inputs`** on such an entry are the viewer's, on demo time: the captions, the waits
  and `{ "hud": "<control>", ... }`, each played as the cursor's trip to the drawn control, as a
  build is. The controls are the replay HUD's (play, speed, back, forward, seek with `"to"`,
  bookmark, leave, sight, follow, jump, the overlays, stats, take over and its picker's row,
  the forks bay, a lane's watch, save and delete) and the top bar's rewind button and the strip's
  (back, forward, the track, Back to now, Play from here).
- **A fork's part**: the game inputs the viewer's fork or takeover goes on with, keyed by the
  fork it belongs to, with the seat its fork takes. **A demo's kept forks**: the forks the
  recording's file already holds when a Take over demo starts, as they were played.
- **A scene's economy**: each seat's storage at the start; income and spending follow from the
  casting's figures.

The keys as built, with every rule both readers refuse, are in [the in-game wiki](ingame-wiki.md),
*The data format*: `"seats"` on a scene, `"game": {end, inputs, sidebar}` on a demo, `"seat"` (the
seat rule, decision 16) and `{ "kill": "<id>" }` on a game input, `"rival"` and `"corpse"` on a unit,
the economy's figures, the viewer's `{ "hud": … }` presses and `{ "until": "seeked" }` wait,
for Rewind `"live": true` on the entry and a demo's `"forks"`, its fork parts, and for Take over
`"takeover": true` on the entry, a demo's `"kept"` forks and a part's `"seat"`. Watching's demos
press the replay HUD's `play`, `speed`, `rew`, `fwd`, `track` (with `to`, and `drag` with `over`),
`bookmark`, `leave` and `leave:no`; Rewind's the live HUD's `rewind`, `rew`, `fwd`, `track`,
`live_play` and `live_now`; Take over's the replay HUD's, its Take over set's `takeover`, `takeas`
(with `seat`), `lanes`, `lane`, `main`, `export` and `delete` (the last three with `fork`) and, after
the takeover, the live HUD's; and for Players `"players": true` and `"screen"` on the entry, a
seat's `"camera"` and `"team"`, the camera input `{at, seat, camera, over}`, a kill's `"by"`, a
type's sight figures, the side layout's `sight` table, and the Players set's `jump`, `sight` and
`follow` (with `seat`), `cursors`, `clicks`, `selections`, `chat`, `stats`, `close` and `chip`
(with `chip`).

## The landings

| # | Landing | Carries | Status |
|---|---|---|---|
| R0 | Research, no code | the resource bar's art and drawing; the side panel while watching; the HUD's inputs; a wiki tick's cost | **done 2026-10-31** (*What R0 found*, below) |
| W | **Watching a replay**: the simulation and the data (C1-C5), the mockup's presentation (F1-F4) and the DLL's (G1-G5), as one landing | below | **landed 2026-10-31** (`a149e0ce`) |
| R | **Rewind a skirmish**: the recorder's rules shared (C1, its own landing), then the simulation, the data, the replay's live mode and the entry (C2-C5), the mockup's frame and the DLL's (F1, G1), the release rule, the threads and Try it's world input in the DLL (G2), and the docs, as one landing | below | **landed 2026-11-01** (`078ac2cb`; C1 earlier, `8005c54f`, `6c976067`) |
| P0 | **A takeover starts the live game running** (the game, its own landing ahead of T1, owner's answer QB = A, 2026-11-01): the takeover ends the playing's pause (`tk_run`), the wiki does not open while a takeover is queued, and the live HUD draws from the frame whose visit armed the fork. MEASURED before it (M0-T, main `587ca2a7`): the live game stood paused at R − 1 = 1500 for 54 s and 68 s until the Pause key, no HUD drew meanwhile, the recording started only at the key, and the toast showed on the live HUD only when the game ran within its 1.8 s | the game's change; tareplay's W checks the game runs with no input | **landed 2026-11-01** (`fa62ae5e`) |
| T1-T3 | **Take over a replay**: a cursor for every seat (C1), the takeover data (C2), the replay's Take over (C3), the entry (C4), the mockup's frame and the DLL's (F1, G1), the threads and Try it in the DLL (G2), and the docs, as one landing | the picker, the Forks bay, the lanes, Save as a replay, Delete, the eviction; a second seat's cursor (decision 16) | **landed 2026-11-01** (`e96fa13f`; *Take over, as built*, below) |
| P0′ | **Sight shows the viewed seat's rates at once** (the game, its own landing ahead of P1-P3, owner's answer OQ-D = A, 2026-11-01): a Sight change lowers the newly viewed record's DisplayTimer `+0xF8`, when at or above GameTime, by whole steps of 30 to the last value of its phase below GameTime, so the draw that follows re-reads that record's resource-bar rates once and leaves the timer in its phase (`bar_rates_due`; [render.md](engine/render.md), *The memo's layout*). MEASURED before it (M0-P2, main `28c1c6e0`): paused at GameTime 300 after Sight 0, full map, 1, full map, 2, full map, 3, full map, the second Sight on seat 1 showed the memo's rates 72.5 / 1.0 / 0.5 / 0, seat 3's, against seat 1's 12.5 / 1.0 / 0.5 / 0, and back on the full map, whose rates are all 0, the bar still read `+72 −1`. A paused seek's rates follow the same engine rule and are left for a later landing after Players, by the owner's choice of 2026-11-01, and so are a takeover that leaves the game paused and a return to now, which change the view around the seek's timer write (render.md, *What the Sight store does not cover*) | the game's change; tareplay's W checks the bar's rates are the viewed record's after Sight seat, full map, seat, full map, and could not run when the two records' rates are equal | **landed 2026-11-01** (`384b36d1`) |
| P1-P3 | Players and the camera | Jump, Follow, Sight (a fog per seat), the overlays, the stats window on the stage (its own chart image) | **landed 2026-11-02** (`b0d8b276`; *Players, as built*, below) |

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
three entries; Leave's question and the watcher's column are
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
  a build or reclaim trip; the release empties the selection the trip was planned from, so the
  rest of the trip would press the MAIN2 the column now shows and lock the game. The release
  therefore drops every generated input not yet fed (`trip_next = ntrip_in`, `genNext =
  gen.length`) with the held state, and the fork drops a timeline's trips only by calling it, so
  one place does it. A release costs the game at most the one order in hand: a trip it cuts off,
  or a building or reclaim armed and not yet used.
- **A live build or reclaim selects its unit on its own tick** (the live data rule, a load
  refusal: `tagpu_wikidata.c` `live_orders`, `sim.js` `checkLiveOrders`). In a live entry each
  build naming a type and each reclaim, in seat 0's game script and in every fork part, has on
  its own tick and before it the click that selects its builder or reclaimer: a move, then select
  pressed, then select released. It holds **by construction**: the release runs only between two
  ticks (Back to now and the fork call it between steps, and a re-simulation applies a logged
  release at its tick before the step), so it cannot fall between that click and the order.
  Reselecting before each order on an earlier tick does not close it: a release can still land
  between the select and the order, and the order then finds no RECLAIM or build button on MAIN2
  and stops the game. **A live entry has no pick**: its popup stands only while reclaim is armed,
  and with the sidebar's trips a reclaim arms only when its trip clicks RECLAIM, ticks after the
  reclaim's own, while the trips' data rule keeps every other input out of the reclaim's tick
  after it and out of its 90-tick trip. A pick is therefore always on a later tick than its
  reclaim, a release between them closes the popup, and the pick trip stops the game; the loader
  is not given the layout, so it refuses a pick whether or not the sidebar would be on. The
  refusal names the demo, the fork part and the tick.
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
`mods` lane holds its live bars (w7) and nothing else of the entry; the eviction is checked by
LIVE_TRACE and shown by no Rewind demo (Take over's demo 4 shows it); the arrow and the
selection rectangle the game draws round the recorded commander in the past are the replay's
overlays of the human seat (MEASURED 2026-11-01, M0-P4: Cursors removed the arrow alone,
Selections the rectangle alone), drawn by the wiki's past view since P1; the live game has no speed of its own
(one game tick a demo step, the game's 1×), and the Pause key during a rewind is not modelled.

**Take over, as built.** One landing again, on Rewind's fork machinery, after the game's own fix
it copies (P0: a takeover starts the live game running, the owner's answer QB). The owner answered
the spec's four questions on 2026-11-01, all with the recommended answer: a takeover entry has no
sidebar (QA); the game is fixed first so a takeover runs on at once (QB); the eviction is shown as
the game shows it at that moment, the picker's warning and the outlined lane, with no watch-again
step (QC); Watching's Try it keeps its Take over button and Forks bay drawn and inert (QD).

| step | what it carries | commits |
|---|---|---|
| C1 | a cursor for every seat in both simulations: the input seats (`nin`), the acting seat, a seat's selection its own units' flags, the seat rule, the seated fork and the per-seat release, hash item 13 | `d654bfa6` |
| C2 | the data: `takeover`, `kept`, a part's `seat`, the seven Take over controls and a press's `fork`, in both readers | `7d45fbac` |
| C3 | the replay's Take over (`replay.js`, `tagpu_wikireplay.c`): watch mode on the selected chain, the picker, the lanes, the switch's two paths, Save and Delete, the takeover into live mode as any seat; `simparity.mjs`'s takeover pass and press traces | `2d6035b7` |
| C4 | `art/wiki/entries/takeover.json` and its four demos, its place in `art/wiki/tree.json` (Replays, between Watching and Rewind), `art/wiki/stage/takeover.field.json` (Rewind's ground), `simcheck.mjs`'s TAKEOVER_CLAIMS | `564c79a4` |
| F1, G1 | the frame in the mockup and the DLL: the record's Take over block and `top_side`, the stage HUD's Take over set, the six toasts, the indexed keys, the press machine's lanes, the top bar of the taken seat's side; `hud.js`'s Take over button down while asked, sought or open; then Back to main recording's cursor | `dd70166d`, `a1c35b4c` |
| G2 | the drawn lanes in `TAGPU_CMD_WIKI`, Try it on a takeover entry in the DLL, the dump's Take over fields, `wiki_regression.py`'s `takeover` lane (t1)-(t5) and the `mods` lane's (w8), both in the gate | `0428fd23`, `a5f8e903` |
| the scene and the pacing | w1's recorded goal moved; `cost()` charging a step whose trip acts its full limit | `fdfdf610`, `e29462e3` |
| D | the documentation pass | the `docs:` commit |

The four demos (`end` 1100, 900, 900 and 1000 demo ticks; every game 2700 ticks, the sidebar
false): **takeover** -- Take over snaps back to R 301 and opens the picker, and Bram's row takes the
game over while Ada's recorded inputs play on; **lanes** -- one kept fork (F1, Bram from 301), the
bay opened, F1's lane switched to without a rebuild, Back to main recording seeking the recording;
**save** -- two kept forks, F2 saved, F1 deleted in two clicks, F2 watched and then deleted while
watched, so the replay goes back to the recording first and the bay closes; **evict** -- three kept
forks, F1 watched and left, Take over at GameTime 1100 snapping to 901 with the warning that it
replaces F2, and Ada's row taking over.

**Decisions taken in the build**, each held by both simulations or both frames and checked:

- **Per-seat input, a seat other than 0 only without a sidebar.** A game simulation holds an input
  state for each input seat -- a takeover entry's every seat of its scene, since Try it may take
  any; any other game's the seats its script and fork parts name; a page simulation one --: the
  cursor, held keys and buttons, the drag, the armed building, the placement gesture, the spacing
  and the reclaim sweep. Every input rule reads **the acting seat**'s (THE ORDERING in
  `tagpu_wikisim.h`: `act` is 0 between calls; an input, a fed script input, a view and the step's
  per-seat tail act as their seat and put it back). A seat's selection is its own units' `sel`
  flags, and a deselect touches only the acting seat's units. The column -- the router, its
  gadgets, its capture and the trips -- is seat 0's, so both readers refuse a seat other than 0 in a
  game whose sidebar is not false (an over-approximation: an absent sidebar may be on), init
  refuses more than one input seat beside a column, and a takeover entry's games must say
  `sidebar: false` (QA). A game of one input seat hashes as before (item 13 is only a game's with
  more than one), so Watching's and Rewind's hash lines did not move.
- **`fork(R, part, mode, seat)` releases the taken seat only.** The inputs not yet fed (every one
  of tick R or later) are split: the taken seat's recorded future and every kill are dropped (a
  death after R is not fated, decision 8), every other seat's are carried; the new script is the
  carried inputs and the part's, merged by tick, the carried first on a tie. The held-button rule
  is checked per seat over it -- the taken seat's hand from nothing, every other seat's from what
  it holds at R − 1 -- before anything is written. The release lets go of the taken seat alone, as
  the game's takeover leaves its player; seat 0's release also drops the trips not yet fed and
  routes the column to MAIN2. The taken seat becomes the viewed one, whose units' notices play.
  With seat 0 and a script of seat 0 alone nothing is carried, so Rewind's fork is unchanged by
  construction.
- **The recording kills Ada's commander, c0, at 1500, beside her builder b0.** Demo 2 needs the
  original's track to carry a death marker F1 does not: F1 plays from 301, and a fork drops the
  recorded kills after its start. b0, the spec's only kill, is no commander and marks nothing. The
  commander dies on the builder's tick, so the seat is left with the solar and the extractor alone,
  as in Rewind's *Undo a death*. Each order of the two deaths that leaves a seat paying for a unit
  without its commander starves the game, which refuse mode stops, in a mod pack: Bram's commander
  in Escalation, Ada's before her builder in Twilight.
- **w1's recorded goal is (290, 300).** The recording first sent Bram's CORAK w1 to (300, 330) at
  701, inside Ada's ARMSOLAR (5 × 5 cells, x 240..320, z 320..400), where no node of the planner
  stands w1's 2 × 2 body: every leg it walked began with an A* that expanded 1 730 of the map's
  1 827 nodes and found no goal, nine times between ticks 701 and 786 in every simulation that
  played the recording. (290, 300) is beside the same spot and clear of the solar and of c0 at
  (330, 270) at 701; its plans expand 2 to 18 nodes. (340, 330), the other candidate, stands w1
  where *evict*'s F4 sends b0 and moved the misses to b0. The claims read the recorded point from
  the data, so none changed. A scene's points are the scene's to keep reachable: the pacing bounds
  a fill's ticks, not a tick's cost (*The measured costs*).
- **`cost()` charges a step whose viewer's trip acts on its clock tick the most a step runs**
  (`tagpu_wikireplay_cost`, `replay.js` `R.cost`): max(SEEK, WALK, the top speed's 8), 64. Such a
  step's press can start a seek, a switch or a rewind's seek that the same step then plays, which the
  state before it cannot say; charged what the state said, a fill could run up to FILL + SEEK − 1
  game ticks: 128 (MEASURED in one fill of Watching's `track`, of Rewind's `steps`, `play`,
  `death` and `chain`, and of Take over's `save` and `evict`). Every other step is charged
  what the state says. So the cost is never fewer than the ticks the step runs, and a fill runs at
  most max(FILL, 64) = 64 game ticks (MEASURED: 64 at most over every demo of the three entries,
  the page's fill at 8 and at 100 steps a fill). The walk that holds the clock feeds nothing, and a
  step feeds its clock tick unless a wait holds it behind a pending seek, so neither is charged for
  the trip. Try it's presses act between fills, outside steps. Pacing only: no hash moved.
- **Back to main recording's cursor is resolved on the lanes drawn at its press.** The control is
  drawn only on the watched lane, and its press leaves that lane, so a cursor resting on it, and
  the next trip leaving it, found no control and jumped to the rest point. A trip carries the
  timeline watched at its press (the cursor's `watched`, the frame record's `WIKIFRAMECUR.watched`,
  bounded to 0..`RECR_TIMELINES` − 1), and `tagpu_wikiframe_pressed` / `frame.js` `pressed` resolve
  a trip to or from `main` on the lanes as drawn then: open, that fork the watched one. Watching a
  lane is what draws Back to main recording and widens the lane to hold it; the other flags only
  fill. Presentation only: the replay's hash does not take it, and `framecheck.mjs` holds it equal
  at every trip's start, press and release.
- **`top_side`, the top bar's side.** The frame record carries the layout side of the human seat
  (`tagpu_wikiframe_seat_side`: seat 0 the load's side, any other seat the opponent's, the first
  layout side other than the load's, which casts the rival units) in a takeover's live game, and the
  load's side in every other record. The top bar -- PANELTOP, the PANELBOT tiles after it, the
  rects, the colours, the font and the badge's place -- is `top_side`'s; the badge's frame is the
  human seat's colour; the bottom bar and PANELSIDE's column are the load's (decisions 14 and 15 as
  M0-T measured them).
- **The picker's rows dim by `units == 0`**, the game's rule, and a row on such a seat takes
  nothing. Kept forks are live rows of the demo's file, `used` 0, in refuse mode; one whose seat has
  no unit at its R − 1 fails the replay. The takeover carries the top bar's memo from watch mode's
  zeros, so it eases from 0 toward the taken seat's values, and sets the DisplayTimer to R − 1.

**Not closed by Take over** (each also in [the in-game wiki](ingame-wiki.md), *Gaps*): the game
lane plays it on the base game only -- under the five mods the `mods` lane holds the taken seat's
two bars (w8) and nothing else of the entry; a taken seat has no column of its own, so in Try it a
player who takes over orders on the map and cannot arm a building (QA); whether the game's top bar
eases from 0 after a takeover is not resolved -- M0-T read the taken seat's values at the first
read, at least 10 s late, and after P0 one window grab 1.06 s after the row drew METAL 6 893 and
ENERGY 6 917 while the memo's floats read 0.29 s later already held 9 861 / 9 894 -- so the wiki's
ease from 0 is [INFERRED]; why the bottom bar stays the load's when the viewed side changes is
measured, not read in the engine ([engine/ui.md](engine/ui.md)); the viewed seat's contact bits,
which the game's takeover changes, have no counterpart in a takeover entry: `+0x110` bit 9 is the
radar pass's sonar contact, and its clearing on every unit but the taken seat's is the pass
replacing the watcher's all-units bits with the taken seat's own, no unit in its sonar range
([engine/recorder.md](engine/recorder.md), `0x467440`'s row). Left out on purpose: the computer
players the game seats after a takeover (decision 8's line), per-seat columns (QA), the watch-again
step (QC), and the refusals no wiki state reaches (the file worker's queue and tickets, `Cannot
watch F<n>`, `Cannot delete F<n>`, `Take over refused`, `Cannot go back to m:ss`).

**Players, as built.** Built in both the mockup and the game; the landing is in progress. The
stats chart's halo the game seemed to lack was the mockup's error: `hud.js` read the wiki's
`rgb(r,g,b)` seat colours as NaN, so every seat looked dark and took the halo the game rightly
draws for dark colours only (`c5009c94`). The game's own change it copies,
P0′ (Sight shows the viewed seat's rates at once), landed first. The owner's answers, each the
recommended one:

- **OQ-A** (2026-11-01): the entry's screen is smaller than its map; the seats' cameras are
  recorded data; the viewer's camera moves only by Jump and Follow.
- **OQ-B** (2026-11-01): the overlays are drawn in every replay view, the Players controls inert
  on every other entry.
- **OQ-C** (2026-11-01): radar contacts as the game makes them.
- **OQ-D** (2026-11-01): Sight shows the viewed seat's rates at once, by fixing the game (P0′).
- **The bar's rates after a paused seek, a paused takeover and Back to now** are left for a later
  landing, and **health bars** are a gap of every entry, not of this one.
- **OQ-E** (2026-11-02, after C1): a layout stores the sight tables only for the radius indices
  its setup's types reach, sparse by index, with no cap on `numtables`; every one of the six
  setups keeps its tables.
- **OQ-F** (2026-11-02, after C4): TA Twilight (sight 600 to 650 on a field 1 536 wide) and TA
  Zero (radar equal to sight) cannot show the Sight demo's hidden units or its radar-only
  contact. The captions are worded to be true in every mod, and the two claims are checked only
  in castings whose figures make them observable, `simcheck.mjs` naming each skip.

| step | what it carries | commits |
|---|---|---|
| C1 | the data: players entries, `screen`, the seats' cameras and teams, the camera input, a kill's `by`, the Players set's controls, the view rule, the death bound, the castings' sight figures and model heights, the side layout's sight table | `9aca7124` |
| G1, engine-free half | `HOP_TRI`, the second chart slot, the world target's scissor | `3ddabfd7` |
| C1b | the sight tables sparse by index, only the ones the setup's types reach (OQ-E) | `32cb95b9` |
| C2 | the simulation: kills and losses, the deaths, the clicks, `moved`, a seat's lit grid and contacts, the army value, hash items 12 and 14 | `d08a1007` |
| C2b | the deaths held to the engine's 20 live temporary sight records | `9256cd20` |
| C3 | the replay layer: the camera, Sight with the bar's phase rule, the pass, the switches, the samples, the players hash block; `simparity.mjs`'s players fixture and PLAYERS_TRACE | `2e0ca4b0` |
| F1 | the mockup's presentation: the screen and the eye, Sight's fog, units and minimap, the overlays, the Players set and the stats window on the stage | `a1096fc0` |
| C4 | `art/wiki/entries/players.json` and its five demos, its row in `art/wiki/tree.json` (Replays, after Rewind), `art/wiki/stage/players.field.json`, `simcheck.mjs`'s PLAYERS_CLAIMS | `8a9bd954` |
| OQ-F | Sight's captions true in every mod; the two figure-bound claims skipped by name | `2748c214` |
| Rewind's goal | `rewind/steps`' move to (400, 250), which every casting's commander reaches | `e569e589` |
| G1 | the game draws the entry as the mockup does: the record's players block, the view's eye and overlays, Sight, the stage HUD's Players set and stats window in chart slot 1 | `55e7195a` |
| the fill | a footprint's metal coverage from sums built at load | `2dcbb41a` |
| the skips | a radar-contact skip names each type's radar and sonar apart | `1945e8fb` |
| G2 | Try it on the Players set in the game, the `players` lane (p1)-(p5) in the gate, the image's clip in `TAGPU_CMD_WIKI` | `0c551df1` |
| D | the documentation pass | the `docs:` commit |

The scene: a 96 × 48 field with a 64 × 30 screen, three seats -- Ada and Cleo of ARM on team 0,
Bram of CORE on team 1 -- each with a recorded camera; Take over's six types with their FBIs'
figures (ARMCOM and CORCOM sight 290, radar and sonar 400; ARMCK 235, ARMMEX and ARMSOLAR 210,
CORAK 275). Every demo's game is 1 800 ticks: Ada builds a solar and sends her commander east,
her camera gliding after it; Bram's w1 goes west and his commander east, his camera following;
Cleo builds a solar and queues an extractor and a second solar; w1 dies at 1100 by Ada and
Cleo's scene solar s2 at 1500 by Bram. The five demos (`end` 600, 700, 640, 620 and 640 demo
ticks): **jump** -- Jump on Bram's row, then Ada's; **follow** -- Follow on Bram's row, and again
(Free cam); **sight** -- the track to 930, Sight on Bram through w1's death vision and Ada's
commander as a radar contact, and again (the full map); **overlays** -- Cursors, Clicks and
Selections off, Cursors on; **stats** -- the window opened, army, kills, the team split, closed.

**Decisions taken in the build**, each held by both simulations or both frames and checked:

- **Sight's top bar is the viewed seat's, its rates by the game's phase rule.** A Sight change,
  held while a seek is pending, re-reads the newly viewed record's rates once and keeps its
  30-tick phase, `bar_rates_due` copied; a seek's end sets every DisplayTimer to the shown tick.
  The memo eases from 0 toward the seat's stored values.
- **The pass runs on the record's deadline**: at a Sight change, a seek's end and on every played
  tick that reaches a multiple of 30, at 2× to 8× as at 1×, as the game's radar pass runs on the
  viewed record's 30-tick deadline `+0xF0`, checked every tick ([engine/recorder.md](engine/recorder.md),
  `0x467440`), while the minimap's dots move every tick.
- **The deaths are bounded as the engine bounds them** (C2b): the engine keeps one list of at
  most 20 live temporary sight records, filled only for the viewed player's deaths, so a 21st
  death within the 61 ticks a record lives is lost in the game; the wiki drops it the same way,
  and the data rule keeps every authored script under it.
- **The sight tables are sparse** (OQ-E, C1b): only the radius indices a setup's types reach,
  so Twilight's 94 tables cost 11 stored, 15 212 cells.
- **The view rule holds every tick, not every move**: a camera may glide under a resting pointer,
  and the game records a seat's cursor on the world only on its player's screen.
- **The stage HUD's chart is its own slot** (slot 1), so the replay HUD's stats window and the
  stage's never share an image.
- **The overlays switch only on a players entry**: the switches are values of every replay, all on
  outside a players entry, so the watching frames of Watching and Take over gained every seat's
  cursors, clicks and selections, and Rewind's past the human seat's.
- **Demo 3's wait stands at 57** (the track's trip runs to its release at 30 + 26) and **its
  track goes to 930**: from 1040 w1's death vision was gone long before the caption naming it;
  from 930 it falls round that caption, and the first radar-only pass, at 1170, before the radar
  caption. **Demo 4's start view** holds w1, Cleo's units and the three cursors. **Cleo's queue**
  spends more than a cut seat's 200 in every pack, for the stall claims every game demo carries.
  **The killed solar is s2, a scene unit**, since a kill names one.
- **Rewind's `steps` goal moved to (400, 250)**: TA Zero's 3 × 3 commanders could not stand at
  (400, 300) beside w1, so every leg searched the whole grid (964 µs a fill, 69 µs at the new goal,
  MEASURED native -O2); `simcheck.mjs`'s goals' claim holds in every context again.

**Not closed by Players** (each also in [the in-game wiki](ingame-wiki.md), *Gaps*): the stats
the game lane plays the entry on
the base game only; health bars are drawn in no entry; the radar-only claim is skipped in TA
Twilight and TA Zero (OQ-F); the bar's rates after a paused seek, a paused takeover and Back to
now (a later landing). Left out on purpose or [INFERRED]: the replay's playback delay, Follow's
engine ends, a drag of the camera or the stats window, chat, altitude in the stamp, jamming and
stealth, the radar dots' blink, the AI groups' digit under Sight, and the footprint corners
standing in for `0x465AC0`'s model-box points. Two engine defect candidates the data rules keep out
rather than reproduce: radius index 0 reading before the table, and the byte counters' wrap past
about 37 stacked units ([engine/map.md](engine/map.md), *The sight table and the stamp*).

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
  the gate (the 0.5 ms target: *The page against the target*, below). The render
  thread's stage HUD at most 0.029 ms in a frame and the bars 0.201 ms alone (0.038 and 0.268 ms in
  the gate). The `mods` lane's (w7) held the live frame's two bars, `bar.js` given the record's
  numbers, pixel-equal on all six setups. The gate's 38 lanes (22 offline checks, 10 modes, 6
  setups): 37 passed; the page lane's fold check (p3) did not count Rewind's row under Replays,
  and with it counted (`f1cc29ed`) the page lane passed on its own.
- **Take over in the game** (MEASURED 2026-11-01, `wiki_regression.py`'s `takeover` lane at
  1920 × 1080, first as the only lane, then in the full gate): (t1) a watching frame of demo `lanes`
  1152 × 544 with the Take over button 20 units tall and as wide as the transport plate above it,
  the Forks bay at the housing's left end level with the track, and none of the live HUD's
  controls; (t2) demos `lanes` (6 ticks) and `evict` (12 ticks, over watching, the lanes, the picker
  and live) paused, the record -- its Take over block and `top_side` among the fields -- and the
  replay's hash equal `replay.js`'s; (t3) the open picker at the limit (`evict` at 510), F1 watched
  (`lanes` at 310) and Bram taken (`takeover` at 620): the column and both bars pixel-equal to the
  mockup's, the taken frame's `top_side` 1; (t4) Try it on `evict`'s data: the bay opened three
  lanes, a lane and Back to main recording switched (both sought), F2 saved and then refused, Take
  over snapped 1101 to 900 and warned of F2, Bram's row took over as F4 at 901 with F2 gone, a
  click as Bram on Ada's commander selected nothing, one on his own selected it, and a right click
  on the ground moved it 187 units to its goal; a Delete armed and made; the watched F1 deleted,
  the replay going back to the recording first; GameTime and the key ring unmoved; (t5) the seek
  to 2700 in 73 demo steps and 2.6 s alone (78 and 3.6 s in the gate), the switch to F3 sought to
  2100 in 62 (66, 3.3 s), the snap to 1800 in 73 (77, 3.3 s); the takeover's fork 0.004 ms alone
  (0.010 ms in the gate); the stage HUD's two builds at most 0.118 ms in a frame (0.185 ms) and
  the bars 0.001 ms. The page cost the game thread at most 0.516 ms in one frame over 440 frames
  alone and 0.635 ms over 445 in the gate. The `mods` lane's (w8): Take over's first demo at tick
  700 after Bram is taken, both bars pixel-equal to `bar.js` on all six setups, the top bar of
  layout side 1. The lane passed in 343 s alone and 506 s in the gate, where every game lane
  passed.
- **A replay's fills** (MEASURED 2026-11-01 by playing the replay layer with the page's fill,
  `WIKI_REPLAY_FILL` 64, on the DLL's i686 SSE2 build under Wine, on `fdfdf610`'s tree): the worst
  fill per Take over demo 0.092 ms (`takeover`), 0.121 (`lanes`), 0.184 (`save`) and 0.142
  (`evict`); Watching's 0.08 to 0.16 ms, Rewind's 0.15 to 0.21; no tick of a Take over demo over
  50 µs. With w1's old goal the nine ticks of a full-grid A* miss cost 153 to 192 µs each against
  0.91 to 0.98 µs for every other tick, and the fill across them 1.00 ms, the page's worst (t5 read
  1.757 ms in the game then).
- **The page against the target**: in the full gate of 2026-11-01 each replay entry's page cost
  the game thread about 0.57 to 0.64 ms at most in one frame -- Watching's (v5) 0.565 ms over 233
  frames, Rewind's (r5) 0.633 over 503, Take over's (t5) 0.635 over 445 -- against the plan's
  0.5 ms. The replay's own fills are at most about 0.2 ms (above), so most of the frame's cost is
  the page's other work [INFERRED estimate: about 0.4 ms]. The owner judged it fine for the wiki
  (2026-11-01): in single player the wiki pauses the game while it is open (`tagpu_wiki.c`, THE
  PAUSE), and in multiplayer the time touches only the viewer's frame time, never the simulation
  [INFERRED: not measured]. The lanes keep printing the figure beside the target, so a regression
  still shows.
- **Players in the game** (MEASURED 2026-11-02, `wiki_regression.py`'s full gate at G2, the
  `players` lane at 1920 × 1080; every lane of the gate passed): (p1) a watching frame 1152 × 544
  on the 64 × 30 screen, the viewport 1024 × 480 at the eye (0, 0), the image's clip the world rect
  and the image the 1536 × 768 map at the eye; (p3) the column and both bars pixel-equal to the
  mockup's in every held state; (p4) Jump on Bram's and Ada's rows, Follow and Follow again, Sight
  and the switches through device clicks, GameTime and the key ring unmoved; (p5) the seek to 1798
  in 85 demo steps and 3.2 s with Sight on Bram and the stats window open. The page cost the game
  thread at most 0.722 ms in one frame over 383 frames; the render thread's stage HUD at most
  0.250 ms in a frame and the bars 0.184 ms, over 390 frames. The lane passed in 1 939 s. In the
  same gate Watching's page (v5) 0.634 ms over 231 frames, Take over's (t5) 0.584 over 445, the
  page demos' (d5) 1.917 over 1 617, and Rewind's (r5) 1.015 ms over 505 frames with its
  longest seek 87 demo steps and 3.4 s. Those two are the full gate's load: the same tree with
  only the rewind and demos lanes, two at once (`-j 2`), measured r5 0.410 ms over 504 frames and
  d5 0.608 ms over 1 733 (MEASURED 2026-11-02).
- **A players replay's fills** (MEASURED 2026-11-02, wikitest's replay layer timed fill by fill as
  `tagpu_wiki.c` paces them, 64 game ticks a fill, native -O2, the fixture layout, best of 9): the
  worst fill 72 to 76 µs, the mean 33 to 40, the first 46 to 47, init 32 to 40 µs (the sight tables
  are read at load); before the coverage sums of `2dcbb41a` 91 to 95, 47 to 54, 61 to 63 and 24 to
  28. What is left is the simulation's own work a tick: the units' moves 62 % (evacuation 37 %) and
  Quick Build's hover 34 %. The same change took Take over's worst fill from 48-63 to 33-43 µs and
  Watching's from 39-47 to 27-33. A seat's lit grid on the worst scene costs 26.6 µs and its
  contacts 11 µs (C2, native -O2).
- **The packet**: `TAGPU_PK_WIKI` 172 744 bytes, the view `WIKISTAGEVIEW` 167 816 of them with the
  eye and viewport, the overlays and Sight's minimap; the frame record 4 600 with Rewind's live
  fields, Take over's block (the picker, the lanes, the human seat, the fork limit and its
  warning, the chain's last, the fill, the picker's R and eviction, three forks, `top_side`, the
  cursor's `watched`) and the players block (2 896 bytes of it the samples); the head 328 with the
  replay's serial; `try_world` sits in padding. `TAGPU_CMD_WIKI` 10 072, the live strip's drawn
  span (`hud_first`, `hud_G`), the drawn lanes (`nlane`, `lane_id[3]`) and the image's rect and
  clip (`img[4]`, `clip[4]`) included (sizeof on the DLL's compiler, 2026-11-02).

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
through a fork or Back to now. The arrow and the rectangle the game draws round the recorded
commander in the past were measured later, in M0-P4: they are the replay overlays of the human
seat.

## Open

- **Sound at speed**: a re-simulated tick plays nothing (a seek's end marks every sound it
  re-simulated as heard); what the stage plays at 2× to 8× should follow what the game does, which
  is not measured [INFERRED until then].
- **Escalation's `MAX_ID` scan** (*The measured costs*): whether the wiki's host answers `MAX_ID`
  with the highest id spawned is the owner's decision.
- **Chat** and **combat** are out of this program. The `ExplodeAs` explosion is a later polish.
