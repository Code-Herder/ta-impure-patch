# The in-game wiki's replay entries (the plan)

## Status

**Designed 2026-10-30/31 in a design interview with the owner; R0 done, nothing built.** The owner
wants the in-game wiki to teach the replay system and the live skirmish rewind the way it teaches
the building and reclaiming features: demos that play on the stage, and Try it yourself. The stage
shows a small game with a handful of units in a whole game frame -- the top bar, the left sidebar,
the map and the replay HUD -- and the demos operate the replay HUD's controls on it, forks
included, the rewind button at the top right among them.

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
| 7 | The top bar and its numbers | **The mod's own resource bar, its numbers from a non-limiting economy** per seat: storage at the start from the scene, income from extractors and generators, spending at the rate builds draw. It never slows a build, so every existing demo plays as now; **both simulations refuse a demo whose storage would fall below zero**, so a demo that passes would never have stalled in the game. A whitebox bar until R0 maps the engine's art. The rewind button and the sprocket are drawn over it as in the game | Fixed numbers would show metal that never moves while extractors are built; a bar without readouts is not the whole game area. The players column (`HUDSEAT`) reads the same numbers |
| 8 | The other seats after a fork | **They play on from their recorded inputs**, through the same rules on a world that may now differ; a fork's own script may give any seat inputs from T. **A fork drops every recorded kill after T**: a death in a fork comes from the fork's script alone. The text says in one line that in the game the other players become AIs | The game seats an AI for each other player (S4b); the wiki has none and grows none. Idle seats would teach that a fork freezes the opponent. Dropping the kills makes "fork before the commander dies and it lives" hold by construction, as in the game, where a death after T is not fated |
| 9 | The fork rules | **The recorder's own, from its own constants**: forks only at multiples of `REC_REFRESH_TICKS` (`inc/tagpu_recfmt.h`, included, not copied) with the HUD's two-click snap back; at most three live forks; the least recently used evicted, never the original or the one watched, with the HUD's warning; forks of forks with parents. "Save as a replay" shows its toast and keeps the fork in a list of the demo's own | A player meets on the stage exactly what the game does, and a change to the recorder's interval or cap reaches the wiki. The eviction order is one pure function both simulations share |
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
  existing kinds with an optional `"seat"` (0..3) and the new `{ "kill": "<unit>" }`. A unit's
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

The exact keys are W1's to settle in the mockup.

## Landings

| # | Landing | Carries |
|---|---|---|
| R0 | Research, no code (**done 2026-10-31**, below) | the resource bar's art and drawing; the side panel while watching; the HUD's inputs; a wiki tick's cost |
| W1 | Mockup: Watching a replay | `sim.js`: the two clocks, the seats, kills and corpses, the economy and its refusal, seek by re-simulation; `wiki.js`: the theatre layout and the frame (a whitebox top bar, the sidebar, the map, `tools/hudart/mockup/hud.js`'s HUD); the entry's demos |
| W2 | C: the simulation and the data, equal to W1 | `tagpu_wikisim.c`, `tagpu_wikidata.c`, `simparity.mjs` extended; the tick cost measured in the game |
| W3 | In the game: the frame on the stage | the theatre layout in `tagpu_wikilay.c` (`wikilaycheck.py`), the HUD built by `tagpu_hudlay_build` (pure: `tagpu_hudlay.h`) inside the stage, the viewer's presses on its controls in Try it yourself, the `wiki_regression.py` lanes |
| R1-R3 | Rewind a skirmish, the same three steps | the fork rules, the top bar's rewind button, the strip |
| T1-T3 | Take over a replay | the picker, the Forks bay, the lanes, the eviction |
| P1-P3 | Players and the camera | Jump, Follow, Sight (a fog per seat), the overlays, the stats window |

Watching goes first because it builds the frame, the two clocks, seek and the kills, which every
other entry stands on, without forks; Rewind second, the owner's own example, whose fork machinery
Take over reuses.

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
  The engine map gets these facts, verified, in landing B's documentation pass.
- **The side panel while watching** is `<prefix>MAIN2` of the side the level loaded, the
  recording's controlled seat's at full map; Sight does not change it. The viewer controls the
  spectator record, whose unit list is empty, so nothing is selected and no build menu shows
  [INFERRED from the code: no capture of a Watch's panel exists].
- **The HUD's inputs**: `tagpu_hudlay_build` needs no change; the stage needs a second `HUDOPS`
  drawn with the HUD's art and its own hit map. The markers and the stats cover the whole recording,
  ahead of the playhead, so a replay demo plays its game script to the end once before it shows.
  `hud.js` runs only in a page: whether a HUD press's cursor trip is simulation state or
  presentation is W1's to settle.
- **A tick's cost**: every demo, with the units' scripts, natively at `-O2`: the longest 1 068 ticks
  in 3.56 ms, the dearest 1 033 ticks in 4.37 ms (4.78 ms with the DLL's compiler under Wine), the
  worst single tick 33 µs (MEASURED 2026-10-31). A seek needs no snapshots. A seek must count the
  sound events it re-simulated as heard, or they play at once.

## Open

- **Sound during a seek and at speed**: a re-simulated tick plays nothing; what the stage plays at
  2× to 8× follows what the game does, which R0 measures [INFERRED until then].
- **Chat** and **combat** are out of this program. The `ExplodeAs` explosion is a later polish.
