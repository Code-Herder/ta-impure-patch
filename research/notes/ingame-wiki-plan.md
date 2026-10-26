# The in-game wiki in the DLL, planned

**Written** 2026-10-25, before anything of the wiki is in the game. It is built from the mockup
(`tools/wiki/mockup/`), the data (`art/wiki/`), the replay HUD's stack and the pristine executable
(`pristine/TotalA.exe.pristine`). Nothing here has been measured in the game. Every engine fact
carries its address and how it was established: DISASSEMBLED, cited from
[the engine map](exe-reverse-engineering.html), or [INFERRED]. Like [g19f-plan](g19f-plan.html),
this file is folded into [the wiki's note](ingame-wiki.html), `gpu-status.md` and `roadmap.md` when
the work closes, and then deleted.

The specification is [the in-game wiki](ingame-wiki.html): the mockup is 1:1. This file says how
the DLL gets there, and where the engine bounds what can be done.

## Where it starts

**Landing 1, the core, is committed on the wiki branch** (`d695bed3`, with its note `286adaa3`,
and the three fixes below in `d3e9d5ac`), not yet on `main`:
- `tagpu_wikidata.c` turns `art/wiki/` into C structs (`tagpu_wikidata_load_resource`). The data
  is the resource `WIKIPAK`, packed by `tools/hudpak.py --set wiki`. `tagpu_pak.c` is the archive
  reader it shares with the HUD's art.
- `tagpu_wikisim.c` is `sim.js` ported function for function: `tagpu_wikisim_input`, `_step`,
  `_view` and `_free`. Its state lives in a `WIKISIM` the caller owns. It is compiled with
  `-msse2 -mfpmath=sse`: an x87 build differed from `sim.js` in 1196 of 9298 frames (the commit's
  numbers).
- `tools/wiki/simparity.mjs` plays every demo through both simulations and compares them tick by
  tick, the state hash included, natively and as the i686 build under Wine: all 21 demos and 9
  rule cases equal.
- The kit is left for the landing that draws it, and nothing in the DLL calls the simulation yet.

**Track B, the page offline, is committed on its own branch** (landing 3's pure half, below): the
layout numbers in `art/wiki/layout.json`, `tagpu_wikilay.c` and `wikilaycheck.py`.

It names no engine address and touches no engine-facing file. **Its shape is right** and the plan
below builds on it unchanged, with the five requests in *What the core must hold*. Requests 2, 3
and 4 -- the map bound, the order cap and the state hash -- are in `d3e9d5ac`; 1 and 5 are
contracts the landings that call the core keep.

**Landing 1b, the take-off rule shared, is committed on the wiki branch** (`e5b39388`): the game
and the core both call `place_take_offs_one` in `tagpu_placegeo.c`.

## The engine facts this rests on

### What the engine reads as input: four channels

| Channel | Engine sites | Established |
|---|---|---|
| **Messages to TA's window procedure** | The only thing that pushes the key ring and the mouse ring: `WM_KEYDOWN` → `0x4C1D50` at `0x4B5D8E`, `WM_SYSKEYDOWN` at `0x4B5DC3`, `WM_CHAR` → `0x4C1B20` at `0x4B5DEA`, button messages → `0x4C2E30` at `0x4B5F51`. A move pushes no ring entry, but on `WM_MOUSEMOVE` the game thread writes the mouse object's position `+0x196`/`+0x19A` (`0x51F4B6`/`0x51F4BA`) | cited: *Who pushes the two rings* (DISASSEMBLED 2026-10-09) and *The engine's mouse thread* |
| **`GetCursorPos`** (IAT `0x4FC2E4`) | `0x4C2610`: the mouse thread `0x4C2990` stores the result in `+0x196` about every 33 ms. Also `0x41CEE7` (the scroll poll's warp-back), `0x4C24DE` and `0x4C28AA`, plus the wrapper `0x4C2310`, which no `call` names | DISASSEMBLED 2026-10-25: every `call [0x4FC2E4]` |
| **`GetAsyncKeyState`** (IAT `0x4FC350`) | The hotkey sampler `0x4C1B80` (`0x4C1BA3`..`0x4C1C2F`, eight sites) and the keydown translator's Ctrl test at `0x4C1D56` | DISASSEMBLED 2026-10-25 |
| **`GetKeyState`** (IAT `0x4FC2EC`) | One site, `0x425F6E` (`VK_SHIFT`, in the front end), and the thunk `0x49F7D6`; the `GetCursorPos` thunk `0x49F7CA` (`jmp [0x4FC2E4]`) sits beside it, and no `call`, `jmp` or pointer names either | DISASSEMBLED 2026-10-25, the thunks 2026-10-26 |

The import table names no DirectInput library (`objdump -p`). The three polls reach cnc-ddraw's
`fake_GetCursorPos`, `fake_GetAsyncKeyState` and `fake_GetKeyState` (`winapi_hooks.c`). The armed
shield already answers through those same hooks (`tagpu_shield.c`, the file comment).

**What each channel drives, if the wiki left it open:**
- **The rings** carry orders, selection, hotkeys and chat.
- **The position `+0x196`** feeds three things:
  - the scroll poll's edge test, an exact equality on the outermost pixel, read through
    `0x4C2340` at `0x41CEC5` (cited, *The scroll poll*);
  - the in-play mouse handler `0x499200`'s region test and hover;
  - the recorder's cursor sample, `main+0x2CAA` at B2 (cited,
    [perspective streams](tadr-port/demo-recorder-perspective-streams.html) E2).
- **The key polls** drive the arrow scroll (hotkeys `0xF4`..`0xF7` in `0x41CE90`) and the Shift
  test that disarms a building after a Shift placement, at `0x4992D8`.

### How the options sprocket and the replay HUD take input today

- **Both are hit-tested on both paths.**
  - The injected path: `deliver_mouse` in `tagpu_shield.c`.
  - The real path: `tagpu_shield_wndproc`, at the top of cnc-ddraw's window procedure
    (`wndproc.c`), before the shield's armed test.

  The sprocket is tested at the game point `g_ddraw.cursor`, against `trigger_rect`: x =
  W/s − 16 − 28, y = 2, 28 × 28 (`tagpu_menu.c`). The HUD is tested at the client point, against
  the controls of the last frame it drew. Those reach the game thread as `TAGPU_CMD_HUD` in the
  command record, taken by `tagpu_hudinput_take` from `tagpu_packet_pub.c`.
- **A press either one takes is dropped whole**: no message and no virtual key state. Its release
  is taken too (`s_pressed` in `tagpu_menu.c`, `s_down` in `tagpu_hudinput.c`), and on the
  injected path a release always clears the virtual key.
- **The wheel** goes to `tagpu_hudinput_wheel` first: at `wndproc.c` for the real path and in
  `to_game` for the injected one.
- **Neither takes moves, keys or the polls.**
- **The render-options drop-down is an engine GUI screen.** It is loaded with `GUI_Load` and kept
  on top through `main+0x37EA0`, and the engine routes mouse records over its panel.
- **Neither pauses.** `tagpu_menu.c` writes no pause bit (from the source).

### How single player is paused

- **The pause is bit 0 of the u16 `main+0x38A51`.**
  - The frame callback's skirmish branch tests it at `0x496920`
    (`test [eax+0x38A51],bl; jne 0x496969`, with bl = 1).
  - A paused frame skips only the scheduler `0x495230` and the step `0x495490`. The draw at
    `0x4969CD` still runs.

  DISASSEMBLED, re-read 2026-10-25.
- **The Pause key** flips the bit at `0x4960AA`..`0x4960BC` and sends message `0x19`
  (re-read 2026-10-25; cited).
- **The engine pauses single player on its own menus.**
  - The in-game menu's open sets the bit at `0x460E00`.
  - The options opener `0x45CFC0` sets it at `0x45D0A1`.
  - Each does so only when `0x435100(main+0x391E9)`, a bare `mov eax,[ecx]`, is not 3, that is,
    outside a network game.
  - `0x45CFC0` also sets `main+0x37EBE` bit 0 (`0x45D002`), which makes the frame callback skip
    the scheduler, the step, the in-game keys and the scroll poll (`0x496918`).

  DISASSEMBLED 2026-10-25.
- **Impure's own pause writers:**
  - the replay HUD's `tagpu_replay_pause`: the Pause key's read-modify-write without `0x19`, on a
    puppet level only;
  - `rw_pause_guard`, the observer on `0x496920`, which reasserts the bit for live rewind and the
    test hold (`tagpu_replay.c`);
  - `fix_load_unpaused`, which clears it at a level's first tick (`0x49842A`).
- **Every other in-play writer of bit 0 is reached only through input**: the Pause key, the
  in-game menu (`0x460E00`, `0x460A33`), and key `0xE3` at `0x49990E`. The options opener's five
  callers `0x45D7C2`, `0x45DE34`, `0x45E613`, `0x45ED56` and `0x460227` are the options screens
  [INFERRED from where they lie]. The remaining writers lie outside play: the LOADGAME screens,
  the battle room and the loader. `0x495B6D`/`0x495DC2` clear the bit and put it back around a
  step. The census is cited from the engine map; the call sites of `0x45CFC0` were DISASSEMBLED
  2026-10-25.

### Multiplayer

- **A network game never reaches the pause test.** The frame callback takes its network branch on
  `main+0x2A44` bit 0 (`0x4967E7`: `test [eax+0x2a44],bl; je 0x496918`). That branch calls the
  scheduler (`0x4967F3`) and the step (`0x49680C`) and never reaches `0x496920` (DISASSEMBLED
  2026-10-25).
- **A locally set pause bit in a network game would stall only this player** [INFERRED]: the
  scheduler also tests the bit (`0x4953D9`, cited), and the Pause key keeps peers together only
  through its `0x19` broadcast. So the wiki must never write the bit there.
- **A single-player level stays single player while the wiki is open.** Bit 0 of `main+0x2A44` is
  set only by front-end code (`0x426D34`, `0x42700C`, `0x4277DF`, `0x427B02`, `0x427C0F`,
  `0x428478`) and is only ever cleared in play (`0x41EFEC`, `0x450E10`, `0x4523D2`, `0x496AB4`,
  `0x496B38`). DISASSEMBLED 2026-10-25, every store whose operand carries the displacement
  `0x2A44`. The `or word, di` at `0x49982F` stores 4: its function sets `edi = 4` at `0x499603`.
  So a level that is single player when the wiki opens is still single player when it closes.

### Threads and the frame packet

- **The HUD's path, thread by thread:**
  1. The game thread fills `TAGPU_PK_HUD`, a packet area at `hud_off`/`hud_len`, from
     `tagpu_packet_pub.c` inside DrawGameScreen's observer at `0x4969D2`.
  2. The render thread lays it out with the pure `tagpu_hudlay.c`, in `tagpu_hudstate_present`
     (called from `tagpu_overlay.c`).
  3. `tagpu_vk_hud.c` draws it: the base layer before the UI composite, the top layer after it
     (`tagpu_vk.c`).
  4. The controls of the drawn frame go back in `TAGPU_CMD_HUD` (`tagpu_zoom.c`,
     `tagpu_cmd_post`) and are taken on the game thread.
- **No render-thread HUD file names engine memory**, and none is in `thread-split.allow`.
- **The HUD's art loads once.** It is decoded on its own thread and published by one interlocked
  store, immutable and never freed (`tagpu_hudart.h`).
- **The HUD's unit is screen height / 720 times its own scale** (`tagpu_hudstate.c`). That is the
  mockup's wiki unit when the scale is at 100.
- **The engine's mouse thread runs during play.**
- **The game thread's x87 precision is 53 bits.** TA's C runtime calls `_controlfp(0x10000,
  0x30000)` from `0x4EA5B0`, which `0x4E432F` and `0x4E8D0A` call (DISASSEMBLED 2026-10-25). This
  matters only as background: the core already compiles the simulation for SSE2.

## The design

### The capture: the engine sees a player who has let go

**The invariant.** While the wiki is open, every input channel answers as if the player had let
go of every key and button and left the pointer where it was when the wiki opened:

- no input message reaches TA's window procedure;
- `GetAsyncKeyState` and `GetKeyState` answer 0 for every key;
- `GetCursorPos` answers the game point of the opening press, and `g_ddraw.cursor` stays there.

**Why this is an ordering.** The open and the close are each one store of the capture word, on the
game thread, which is the window procedure's thread. They are made under the wiki's cursor lock,
together with the frozen point and, at the open, `g_ddraw.cursor`. `GetCursorPos` is the one poll
that stores what it reads: the engine mouse thread calls it about every 33 ms, and it writes the
live point into `g_ddraw.cursor`, which `tagpu_vpwide.c` hands the engine at zoom ≠ 1. So it tests
the word and makes that store under the same lock (`tagpu_wiki_cursor_store`). A poll preempted
between its read of the device and its store therefore either stores before the open, which then
overwrites the point, or finds the word set and stores nothing. The frozen pair is read under the
lock too, so a close and a reopen never mix two points. The key polls store nothing, so an
interlocked read of the word orders each of them before or after the store. Input given before the
open reaches the game and input given after it does not, so there is no window to lose.

**What follows, each by construction:**
- **No order, selection, hotkey, chat line or wheel notch** reaches the engine, in multiplayer as in
  single player.
- **The world under the window does not react to the pointer.** The frozen point is the opening
  press, on the wiki button in the top bar. `0x499200` finds it on neither the minimap nor the
  viewport (`0x49922C`..`0x499252`), sets `cursornormal` (`0x49925D`) and jumps past the build
  cursor `0x4197D0` and the hover query `0x48CD80` (DISASSEMBLED 2026-10-25). So there is no
  ghost, no hover, and no edge scroll: the button spans bar rows 2..29, never an outermost pixel.
- **The camera holds**: the arrow hotkeys read up.
- **Shift reads up.** After a Shift placement (`main+0x2CC6` bit 5), the test at `0x4992D8` runs
  for every mouse record after the region test and disarms the building, exactly as letting go of
  Shift would. This is stated behaviour, not a defect.
- **The recorder has nothing to take** in a network game: no click at its two sites (`0x4995B3`,
  `0x4993AC`), no change of selection, and a world point that does not change. In single player
  no step runs at all.
- **The window's own keys are taken too**: Alt+Enter, and Alt+F4. Both arrive as `WM_SYSKEYDOWN`,
  which the engine's translator also reads (`0x4B5DC3`).

**The paired press and the paired key.**
- **The engine never misses the release of a press it saw.** What it saw is recorded where it is
  handed a message: both doors into TA's window procedure (the tail of cnc-ddraw's and the shield's
  `to_game`) call `tagpu_wiki_engine_got`, which keeps the left, right and middle buttons and the
  keys the engine holds down. A release of one of those goes to the engine whatever the wiki's
  state: a key's as it is, and a button's, while the window is open, at the frozen point, through
  the injected path's own delivery (`TAGPU_WIKI_FROZEN`; cnc-ddraw's would store the release's live
  point). So a button held across the open is let go where the engine last saw the pointer. The
  auto-repeats of a key the engine holds are taken while the window is open, and owe nothing.
- **A press the wiki takes has its release taken too**, whether the wiki is still open or not,
  unless the engine holds that button or key from an earlier press.
- **The wiki button and the X act on the press**, as the sprocket does.
- **Esc closes on its release, and only the release of an Esc whose press the wiki took.** So the
  press, its auto-repeats and the `WM_CHAR` that `TranslateMessage` posts for it all arrive while
  the wiki is still open, and none of them reaches the engine.

**The cursor.** For every move the wiki takes, it notes the client point the same way `mouse.c`
does (`mouse_client_inside`, then `mouse_note_client` or `mouse_forget_client`). The sharp layer's
cursor therefore follows the pointer while the engine's position stays put. Outside the picture no
cursor is drawn; the packet record says so.

### The pause

- **On open, in a single-player level** (`!(main+0x2A44 & 1)`): remember bit 0 of `main+0x38A51`
  and set it.
- **On close**: clear the bit only if it was clear at the open.
- **At level end**: forget it and write nothing.

Nothing else writes the bit while the wiki is open, because **the window does not open while
another pause is in flight**. Every engine writer in play is behind input the wiki holds, or is a
peer's message `0x19`, which a single-player level never receives (the census in
`exe-reverse-engineering.md`, *A level starts paused*). Two of Impure's writers put a pause back
later without input:
- **The live rewind.** Its open saves bit 0 and sets it; its end restores what it saved. In the
  `FLUSH` and `ENDING` states that end comes from the frame visit, unasked, when the writer
  finishes or the open is refused (for example "no complete seek interval yet").
- **The replay HUD's Leave.** The next frame's observer opens its question, and puts the pause back
  when it cannot, or once the question is gone.

So the wiki refuses to open while `tagpu_replay_rewind_state` is not idle or
`tagpu_hudinput_leave_pending` is set, and logs `wiki: not opened - ...`. Neither starts while the
window is open, since their buttons are the replay HUD's. The one verb that can be queued before
the open and run after it is a rewind's open; it saves the wiki's pause and gives it back at its
own end, so the game stays paused after both have closed, until the Pause key: a pause outliving
the window, never a game running under it. This needs no `0x496920` observer, which lives in
`tagpu_replay.c`; changing that file would run every replay test group, since the gate treats it as
the recorder's own.

A replay is a skirmish, so the wiki pauses the playback there too (question 6). The recording is
untouched: a pause steps nothing, as a Pause key steps nothing.

### Data and threads

| State | Owner | Written by | Read by |
|---|---|---|---|
| open, the opening point, the paired press and key, the pause taken | game thread (`tagpu_wiki.c`) | input hooks and the fill | the fill; **any thread** reads the capture word through an interlocked read, and the frozen point, with `GetCursorPos`'s store, under the cursor lock |
| entry, demo, folds, scroll, playing, speed, try | game thread | input hooks against the last drawn frame's hit map | the fill |
| the `WIKISIM` and its clock | game thread | the fill (`step`) and Try-it input (`input`) | the fill, which copies its view |
| the loaded wiki: `WIKIDATA`, the kit, the faces | published once by the art loader thread, then immutable and never freed | the loader | the game thread (the simulation) and the render thread (layout, text) |
| `TAGPU_PK_WIKI`, values only | the packet | the game thread's fill | the render thread |
| the layout and its ops | render thread (`tagpu_wikistate.c`) | `tagpu_wikilay.c` and `tagpu_wikistage.c`, both pure | `tagpu_vk_hud.c`'s wiki layer |
| `TAGPU_CMD_WIKI`: the hit map, the stage rect, the scroll limits | the command record | the render thread | the game thread (`tagpu_wiki_take`) |

**The simulation runs on the game thread**, inside the packet fill: the place input arrives, and
where the view is copied out by value. The render thread never touches a `WIKISIM`, so the
simulation's lifetime is confinement to one thread.

### Renderer coverage

**On Vulkan only**, behind `tagpu_hudstate_on()`, the replay HUD's own gate. On GDI the button is
never drawn, so there is no hit map to open it from, and the wiki cannot open: by construction, not
by a test. GDI is [the reference for the original game](vulkan-only-plan.html), and the replay HUD
and the rewind button already have nothing there. **The recommendation is no button on GDI**
(question 1).

**A kit the window cannot draw is refused at its load, and at a frame too.** `tagpu_wikilay_load`
refuses a kit missing any part or a role's style, and the art loader then checks the icons and the
`toast` style the window and the stage draw (`tagpu_wikilay_check`), so a kit the page cannot draw
never publishes and no button is drawn. The same check builds the page closed, open with its close
button held, and in every entry's every demo playing at 1× and ended at 2×, at 1024 × 768,
1280 × 720 and 3840 × 1080 with every scroll at 0, and refuses the data if any of those builds drops
an op, run, text, hit or group (`HUDOPS.overflow`). The build itself refuses nothing but a missing
argument or its memory, so the overflow is what those states test; other scrolls, captions, held
keys and screens are not built, and a frame that overflows all the same draws the ops that fit and
says so once. MEASURED 2026-10-26 with `tagpu_wikilay.c` compiled natively over the archive the DLL
embeds, the window open on every entry's every demo at 1024 × 768, 1280 × 720, 1920 × 1080,
3440 × 1440 and 3840 × 1080: the fullest page takes at most 115 of the 4096 ops (at 1024 × 768),
47 of the 2048 runs and 2133 of the 32 768 text bytes. Should a frame's layout refuse all the same (its build's own memory), the render
thread publishes it
(`tagpu_wikistate_refused`), and the fill closes the window as it does when the HUD's pass stops:
the wiki is never open where it cannot be seen.

### Replay and simulation classification

- **Every wiki file is `presentation`.** None is marked `sim`, and none declares `SIM_*` state. The
  wiki's simulation is a stand-in played for the player; TA's simulation never reads it, and the
  recorder must not carry it.
- **The recorder needs no change.** It carries nothing of the wiki, and the effects the wiki has on
  the game behave as follows:
  - **The pause.** Bit 0 of the pause word is on no row of `replay-globals.tsv`; only bits 1 and 2
    are, as `leave`. A paused frame steps nothing, so nothing is recorded.
  - **The capture.** Captured input never reaches the recorder's click sites or its selection
    check, and the cursor sample does not change.
- **The only replay gate this work triggers** comes from fixture `sources`. `tagpu_packet.h`,
  `tagpu_packet_pub.c` and `tagpu_menu.c` are sources of the seventeen `resource-*` test groups
  (`tools/replay/fixtures.json`), so landing 2 runs them through `tareplay.py landing`. That is the
  right test: they prove a closed wiki takes no click of theirs.
- **Two RECORDER_OWN paths are kept out of every landing**: `tagpu_replay.c` and `tools/tacli`.
  Either would run every test group. The wiki's test reads its own trigger dump, so it needs no
  `tacli` command.

### Rule code: what the wiki's simulation shares with the feature

**`tagpu_placegeo.c`.** Already engine-free and already shared: the core calls `place_shape` and
`place_ring`.

**`tagpu_place.c` `take_offs`.** Almost pure already. Its body reads only the `s_plan` snapshot
(values), the sites, the footprint and `c->nb` (read 2026-10-25, after the take-off fix
`34b1b586`). The core's port, `take_offs_one`, is the same rule cut per builder, because its commit
judges each builder against its queue as it stands at that builder. So the shared function is the
per-builder one:
- `int place_take_offs_one(const PlaceOrder* q, unsigned nq, const PlaceSitePos* s, unsigned n,
  uint16_t* by, uint8_t* took)`, in `tagpu_placegeo.c`: one builder's orders, each with its order
  position in world units and whether it is of the build action, and each site's order position
  and whether it is refused. It writes `by` (per order) and `took` (per site), and returns how many
  sites took one. `within` moves with it.
- `tagpu_place.c`'s `take_offs` loops builders over `plans_of` and calls it; `toggles` stays.
- The core's `take_offs_one` becomes a call, after converting its positions to the same units.

Its cost:
- **the replay gate**: one test group, `place-basic`;
- **the review**: high, since this is the commit path of an order-writing feature;
- **the tests**: `placetest` and `place_regression.py core`.

What it buys: the take-off rule, the most intricate rule the wiki mirrors and freshly fixed, is one
function in the game and the wiki. **Recommended**, as its own small landing after the core
(question 3).

**`tagpu_resource.c`** (`site`, `snapped_site`, `distinct`, `same_plan`, `find_cancel`, `queued`,
`quick_choice`, `plans_block`, `fits`): **not recommended.** Every one of them reads engine memory
directly:
- definitions at stride `0x249`, unit records, the feature grid's `+7` byte and the mapped bits;
- order-node chains, with their `+0x42` flags `0x400000` and `0x40000`;
- the engine's site test, through its call site.

Sharing them means rewriting the shipped feature over an interface of callbacks (metal, seen,
footprint, kind, strength, fits, plans, units, `can_build`). That needs a high review and all
seventeen `resource-*` groups, puts an indirect call in its per-mouse-record search, and still
leaves the wiki's simplified site test and orders as its own. **Instead, a drift check:**
`tools/wiki/rulecheck.py` hashes the comment-stripped body of every function that `sim.js` and
`tagpu_wikisim.c` name as mirrored, against hashes recorded beside the port. It fails, naming the
port's function, when the feature's code changes. Like `laycheck.py`, it notices disagreement
without coupling the two.

### What the core must hold

1. **A view is a picture.** `WIKIVIEW`'s pointers into the `WIKISIM` are valid only until the next
   call. The fill copies them, bounded, into the packet; nothing keeps a pointer.
2. **Bound the map for the packet.** `MAP_MAX` is 512 cells a side, but the record carries the map
   by value. Lower it to 128 (the data's map is 40 × 24): at 128 cells a 700-pixel stage still
   gives 5 px a cell, and the record's map is then at most 32 KB.
3. **Bound every order list.** `sim.js` caps a builder's list at 1024 (`PLACE_LIST_CAP`) only in
   `commit`. The Shift appends of `quickOrder`, `moveOrder` and the assist path grow without bound,
   and in C that is `realloc` until `oom`, in a 32-bit process, from a player's clicks. Cap every
   append at 1024 in both files. The cap changes `sim.js`, so `simcheck.mjs` gains the rule.
4. **A state hash**, `tagpu_wikisim_hash(const WIKISIM*)` (FNV-1a over the view and the orders in a
   fixed order), with its twin in `simparity.mjs`, for landing 4's in-game check.
5. **The data it points into is the published copy**: loaded once, never freed (landing 2). The
   simulation runs on the game thread, and must keep the thread-free shape it already has.

## The landings

**The boundaries come from the engine.** Landing 2 carries every engine-facing piece: the capture,
the pause, the packet hook and the cursor's redraw. After it, the wiki grows in files that name no
engine address. The packet record's contents live in `src/tagpu_wikipk.h`, which no test group
names as a source. Three `sim` files include it through `tagpu_packet.h` (`tagpu_patches.c`,
`tagpu_weapons.c`, `tagpu_datakeys.c`) and none reads a type of it, so what the wiki draws and
takes changes nothing they read. Its area is appended **last** in the packet, after
the font area, so it cannot truncate or move any table before it. Changing the record therefore
affects nothing earlier in the packet. Confirm with `tareplay.py landing --check` before each
commit.

### Landing 1 — the core (committed on the branch)

**Review: medium** (it touches `tagpu/ddraw/**` and adds a resource and build flags, but no engine
state). It also changes `tagpu_hudart.c` to read through `tagpu_pak.c`, so the review checks that
the HUD's art still loads. **No replay gate is expected**: neither the Makefile nor `res.rc` is a
fixture source, and nothing calls the code yet; `tareplay.py landing --check` confirms.

**Status: the order cap (3), the map bound (2) and the state hash (4) are in, `d3e9d5ac`.**
- The cap: every append to a unit's orders goes through `push_order`, which refuses past
  `PLACE_LIST_CAP`; a click's orders go in whole or not at all, so an upgrade's reclaim never goes
  in without its build. `sim.js` caps the same appends, a scene's own orders included, and
  `simcheck.mjs` has the rule case *every Shift append stops at 1024 orders* (it fails on the
  `sim.js` before the change).
- The bound: `WIKI_MAP_MAX` 128 in `tagpu_wikidata.h`, refused by the loader.
- The hash: `tagpu_wikisim_hash`, 64-bit FNV-1a, its fields, order and encoding stated in
  `tagpu_wikisim.h`; `simparity.mjs` has its twin, compares it on every tick and after every view
  of a rule case, and `--hash` prints each demo's final hash.

MEASURED 2026-10-25: `simparity.mjs` native and `--wine`, all 21 demos and 9 rule cases equal;
`simcheck.mjs`, every demo and rule passes; `make` and `make LIMITS=stock` clean. The launch with
the replay HUD showing (the third check below) is not run.

**Verification:**
- `node tools/wiki/simparity.mjs`, natively and with `--wine`;
- `node tools/wiki/simcheck.mjs`, with the new cap's case;
- one launch of the built DLL with the replay HUD showing, which proves the HUD's art still loads
  through `tagpu_pak.c` (`tacli hud`, 2 minutes).

### Landing 1b — the take-off rule shared (optional, question 3)

**Files:**
- `src/tagpu_placegeo.c`/`.h`: `PlaceOrder`, `PlaceSitePos` and `place_take_offs_one`.
- `src/tagpu_place.c`: `take_offs`'s loop body replaced by a call.
- `src/tagpu_wikisim.c`: `take_offs_one` replaced by a call.

**Invariant:** the function reads only its arguments. The comment says the preview's colours and
the commit's removals both read what it leaves.

**Verification:**
- `tests/placetest.py`;
- `place_regression.py <instance> core --launch` (5 min);
- `tareplay.py landing`, the `place-basic` test group;
- `simparity.mjs`.

**Review: high.** It can run beside landings 2 to 4.

**Status: committed on the wiki branch, `e5b39388`.** `tagpu_place.c` keeps its snapshot in three
arrays indexed alike (`s_plan`, `s_order`, `s_by`), so one builder's orders are the contiguous run
the function reads, and `take_offs` calls it per builder; `place_within` and `PLACE_LIST_CAP`
moved to `tagpu_placegeo.h` with it. The core converts its double positions to 16.16 exactly and
clamps an order to ±8192 world units, which changes no answer since a site the rule compares is
inside the map (a `_Static_assert` on `WIKI_MAP_MAX` holds it); its scratch is fixed arrays of
`PLACE_LIST_CAP`. `placetest.c` has the rule's table cases.

MEASURED 2026-10-25: `placetest.py` 657302 checks, 0 failed; `place_regression.py --selftest`;
`simparity.mjs` native and `--wine` equal; `make` and `make LIMITS=stock` clean;
`place_regression.py <instance> core --launch` on a private Xvfb, (a)–(n) passed with the same
orders and counters as before the move (`commits 11, issued 64, removed 4, red 3, mismatch 0`);
`tareplay.py landing` over `main..e5b39388` passed, 18 test groups (`place-basic`, the sixteen
`resource-*` and `stock-resource-mex` -- the branch's range carries more than this landing), no
check failed, its `replay-roundtrip:` note on `e5b39388`. The high review is not run.

### Landing 2 — the button, the window, the capture and the pause

**Status: built and verified 2026-10-25 on its worktree branch, not landed.** As built, against the
list below:

- **The shell was laid out by a file of its own**, `tagpu_wikiframe.c`, while the page's
  `tagpu_wikilay.c` was built offline. The integration of landings 3 and 4 retired it:
  `tagpu_wikilay.c`, the layout `wikilaycheck` covers, lays out the window, its title and close
  button, the button and the page, with the hot keys `window`, `close` and `wiki`.
- **`tagpu_wiki_mouse` takes the game point as well** (`code, cx, cy, gx, gy`): the open stores the
  frozen point from it, so `g_ddraw.cursor` is the opening press on the injected path too. On the
  real path the open converts the press's own `lParam` with `mouse_client_to_game`, because
  `g_ddraw.cursor` is not the pointer while the mouse is unlocked (windowed, after the focus came
  back: no move reaches it then).
- **The release that re-locks the mouse goes on once the window is closed.** With the mouse
  unlocked, cnc-ddraw's window procedure takes a button's release to lock it and hands the engine
  nothing, so the wiki lets a paired release through then. While the window is open it takes it all
  the same, since the lock would store the release's point in `g_ddraw.cursor`; the next click after
  the close locks the mouse, as any click does while it is unlocked.
- **The window also takes `WM_SYSKEYDOWN`/`WM_SYSKEYUP`** (question 9), and **`WM_MOUSELEAVE`**
  goes no further than forgetting the client point while the wiki holds input.
- **The record's `paused` means the wiki holds the pause**, and `pause_was` that the bit was
  already set at the open: the close clears it only when `pause_was` is 0.
- **The wiki's area is the packet's last**, after the effects' models.
- **The PAUSED icon (risk 8, question 10)**: in single player at 1920 × 1080 and 3840 × 2160 the
  engine's `PAUSED` is under the window and does not show in the capture. Question 10's
  recommendation (a "Paused" chip in the title groove, in the mockup first) is the owner's call;
  nothing of it is built.
- **Verified** with `tools/wiki_regression.py`: sp on Xvfb at 1920 × 1080 (every check, 78 s) and
  at 3840 × 2160 ((a) to (f)); recorded (31 s); mp, two retail peers on Wine on their own DirectPlay port (122 s); `make` and
  `make LIMITS=stock` clean; the replay gate, `tareplay.py landing`, passed on `c96cf852` with
  34 test groups in 14 minutes, every check passing.
- **The gate is broader than verification 5 expected.** `tagpu_packet.h` is included by three `sim`
  files (`tagpu_patches.c`, `tagpu_weapons.c`, `tagpu_datakeys.c`), so the header's change names
  every feature of theirs, and with the wiki branch's own changes against `main` the gate ran
  34 groups, not the `resource-*` ones alone. `tagpu_wikipk.h` reaches the same files through it.
  A later landing that changes only `tagpu_wikipk.h` therefore runs the same groups.
- **Not run**: the wiki in a replay (question 6) has no test; the real input path is the owner's
  hand-over (risk 3); 3440 × 1440 and 1024 × 768.

**What the player sees.** A 28 × 28 wiki button sits in the top bar's right-hand row, 3 bar pixels
left of the rewind button, or in the rewind button's slot when that is absent. A press opens the
kit's window frame (90 % of the game screen, centred), with the title "Wiki" and its close button.
The body is empty until landing 3.

While the window is open:
- the game takes no input;
- single player is paused and multiplayer runs on.

The window closes on the X, on the wiki button, and on the release of Esc.

**New files:**
- **`src/tagpu_wiki.c`/`.h`** — **game thread**, apart from two words any thread may read. The
  entry points:
  - `int tagpu_wiki_message(UINT msg, WPARAM wp, LPARAM lp)`: the real path, called from
    `tagpu_shield_wndproc` after the `WM_TAGPU_*` cases and before the sprocket and HUD block.
    Returns 1 when the message is taken. While open it takes every message on the shield's
    swallow list: keys, chars, `WM_INPUT`, moves, buttons and the wheel. While closed it takes a
    press inside the wiki button's rect of the last drawn frame, in client pixels from `lParam`,
    and the paired release or Esc release.
  - `int tagpu_wiki_mouse(int code, int cx, int cy)`: the injected path. It is called in
    `deliver_mouse` after the `TAGPU_M_DEV` conversion and **before** the store to
    `g_ddraw.cursor`. `cx < 0` means no client point: such an injection is taken whole while open
    and passed while closed, as the HUD does.
  - `int tagpu_wiki_key(unsigned vk, int up)`, called in `deliver_key` before `set_key` and
    `tagpu_place_key`, and `int tagpu_wiki_char(void)` for `WM_TAGPU_CHAR`.
  - `void tagpu_wiki_cancel(void)`, on `WM_KILLFOCUS`, `WM_CAPTURECHANGED` and `WM_ACTIVATEAPP`
    off: the paired press stays owed.
  - `LONG tagpu_wiki_holds_input(void)`, `void tagpu_wiki_frozen(LONG* x, LONG* y)` and
    `int tagpu_wiki_cursor_store(int x, int y, int outside, LONG* ax, LONG* ay)`: **any thread**.
    An interlocked read of the capture word; the opening point as one pair, and `GetCursorPos`'s
    store of a live point, both under the cursor lock the open and the close hold
    (`tagpu_wiki_init`, at attach, makes it).
  - `int tagpu_wiki_fill(TAGPU_PK_WIKI*)`, `void tagpu_wiki_take(const TAGPU_CMD_WIKI*)` and
    `void tagpu_wiki_level_end(void)`.
  - The trigger `tagpu_wiki.dump` → `tagpu_wiki.json`: open, the pause taken, the hit map, the
    state. It is written from the fill on the game thread, as the HUD's `tagpu_replayhud.dump`
    is, and it is what the test reads.
- **`src/tagpu_wikipk.h`**: `TAGPU_PK_WIKI` and `TAGPU_CMD_WIKI`, values only. In this landing:
  - `open`, `paused`, and `held`: the control a left press holds down, which the layout draws
    pressed (the wiki button and the close button among them);
  - `ptr_x`, `ptr_y`: the client point of the fill's last move, −1 for none or outside the
    picture;
  - the hit map, 160 hits of {`x0`, `y0`, `x1`, `y1`, `key[24]`}, and `drawn`.
- **`src/tagpu_wikilay.c`/`.h`**: **pure C99**, no Windows header, so a native tool compiles it
  (`tagpu_hudlay.c`'s shape). The mockup's layout, emitting `HUDOPS` in the wiki unit u = screen
  height / 720 device pixels. In this landing:
  - the window: share 0.90, centred;
  - the title groove, the close bay and the body inset, from the mockup's `GEOM.kit`;
  - the wiki button in the bar's own pixels: k = top / 32, as `tagpu_hudlay.c` `live` places the
    rewind button. Its x is W − (78 + 28)k when this frame draws the live HUD's rewind button,
    else W − (47 + 28)k; y = 2k; side 28k;
  - the hot keys `wiki`, `close` and `window`.
- **`src/tagpu_wikistate.c`/`.h`**: **render thread**.
  - `tagpu_wikistate_present(f)` runs beside `tagpu_hudstate_present` in `tagpu_overlay.c`. It
    turns the record into the layout's state, builds the frame's ops, and reads `live` from the
    same frame's `HUDSTATE` (`tagpu_hudstate_frame`) for the button's slot.
  - `tagpu_wikistate_cmd(TAGPU_CMD_WIKI*)` runs beside `tagpu_hudstate_cmd` in `tagpu_zoom.c`.
- **`tools/wiki_regression.py`**: the game test, after `place_regression.py`. It imports
  `resource_regression`'s `tacli_api` and `Check`, holds the run lock under `--launch`, and has the
  modes `sp`, `mp` and `recorded`.
- **The kit, fitted.** `art/wiki/kit/` as cut moves to `art/wiki-full/kit/`. A `--set wiki` mode
  of `tools/hudart/fit.py` writes the fitted `art/wiki/kit/`, at 3 device pixels a unit (3840 ×
  2160) for the window's parts and 4.5 for the button (the bar at 4K and 150 %). `WIKIPAK` gains
  `kit/kit.json` and `kit/parts/*.png`. The parts as cut decode to 22.6 MB of RGBA; fitted they
  come to about a tenth of that [INFERRED from the scale].

**Changed files:**
- **`src/tagpu_shield.c`**: the four hooks above, and the cancel.
- **`src/winapi_hooks.c`**: `fake_GetCursorPos`, `fake_GetKeyState` and `fake_GetAsyncKeyState`
  check the capture word first. While it is set, the cursor answer is the frozen point (no store
  to `g_ddraw.cursor`) and every key reads 0. `fake_GetCursorPos` makes its store through
  `tagpu_wiki_cursor_store`, which tests the word again under the cursor lock.
- **`src/tagpu_menu.c`/`.h`**: `int tagpu_menu_single_player(void)` (`main+0x2A44` bit 0 clear)
  and `int tagpu_menu_pause_hold(int on, int* was)` (the Pause key's read-modify-write of bit 0,
  without `0x19`). Its `thread-split.allow` argument grows by that read and that write
  (question 2).
- **`src/tagpu_packet.h`**:
  - `wiki_off` and `wiki_len` in `TAGPU_PACKET`;
  - `TAGPU_CMD_WIKI wiki` in `TAGPU_CMD`;
  - `TAGPU_PK_TRUNC_WIKI` and `tagpu_pk_wiki()`;
  - it includes `tagpu_wikipk.h`.
- **`src/tagpu_packet.c`**: the acquire checks `wiki_len == sizeof(TAGPU_PK_WIKI)` and `area_ok`,
  as it does for the HUD's area.
- **`src/tagpu_packet_pub.c`**: the wiki area is appended after the font area, so it is the last
  area; `tagpu_wiki_take` sits beside `tagpu_hudinput_take`.
- **`src/tagpu_reclaim.c`**: `tagpu_wiki_level_end()` beside `tagpu_hudinput_level_end()`.
- **`src/tagpu_hudart.c`/`.h`**: the same loader thread loads `WIKIDATA`
  (`tagpu_wikidata_load_resource`) and the kit, and publishes them with a **second** interlocked
  store (`tagpu_wikiart_get()`). The wiki can then fail on its own while the HUD stands. The kit is
  parsed by `tagpu_hudlay.c`'s part reader, factored so that it can read a kit with no layout.
- **`src/tagpu_vk_hud.c`/`.h`**:
  - the kit's images are uploaded with the HUD's;
  - `tagpu_vk_hud_record_wiki()` is a third layer, after the top layer, built through the same
    `tagpu_huddraw` (one glyph atlas, emptied only before the frame's first build, which covers a
    third build);
  - `prepare` answers 1 when the HUD or the wiki draws.
- **`src/tagpu_vk_gui.c`/`.h`**: `tagpu_vk_gui_record_cursor()`.
  - **What it is**: this frame's cursor quad (the sharp layer's cursor, recorded last among its
    quads), drawn again straight onto the attachment. It uses a `CURS_FS` pipeline built against
    the main render pass, as `s_pipeLay` is, and maps through `lay_viewport`.
  - **Why it is needed**: re-drawing the sharp layer inside a rect, which is what the HUD's top
    layer does, would bring the sharp minimap and strings over the window.
- **`src/tagpu_vk.c`**: the order becomes HUD top layer → the Config panel and the sharp layer
  inside the top rect → **the wiki layer → the cursor record**, when the wiki layer drew, and when
  the record's pointer is in the picture and not hidden → scaffold → readout.
- **`Makefile`**: the new objects, and `tagpu_wikilay.c` in the native check's sources.

**Who reads and writes what:**
- **Engine reads, all on the game thread through `tagpu_menu.c`**: `main+0x2A44` bit 0, and bit 0
  of `main+0x38A51`.
- **Engine writes**: bit 0 of `main+0x38A51`, only in a single-player level, only by
  `tagpu_menu_pause_hold`, only at the open and at the close.
- **Nothing else in engine memory**, in either direction.

**The comments that must stand:**
- **`tagpu_wiki.c`, at the top:**
  - GAME THREAD;
  - the capture invariant, naming the four channels and the import table they were taken from;
  - the ordering argument for the open and the close;
  - the paired press and the paired Esc;
  - Shift reading up, with `0x4992D8`.
- **Above the pause**: the network-branch bound, with `0x4967E7`, `0x496920` and the writers of
  `0x2A44` bit 0 (set only in the front end, cleared only in play); why no other writer can undo
  the pause while the wiki is open; and that level end forgets the pause without writing.
- **`winapi_hooks.c`, at the gate**: the engine's mouse thread calls `fake_GetCursorPos`, and the
  cursor lock orders each poll's store before or after the open.
- **`tagpu_menu.c`'s two accessors**: the DISASSEMBLED facts, with addresses.
- **`tagpu_wikipk.h`**: values only; appended last; the bound of every array.
- **`tagpu_vk_gui.c`'s cursor record**: it draws this frame's resolved cursor quad and never reads
  the pointer live.

**Verification**, each run ten minutes or less:
1. **The build**: `make -C tagpu/ddraw -j$(nproc)`. Its `thread-split-check` must pass with only
   `tagpu_menu.c`'s argument grown.
2. **`tools/wiki_regression.py <instance> sp --launch`** on `scenarios/place-basic.json`, held at
   GameTime 120, about 5 minutes. The shield is armed, so device injections walk the injected path
   (`dmove:`/`dclick:`).
   - **Closed.** The dump names the button in the rewind slot. A device click on the world selects
   and deselects as before.
   - **Open.** A device click on the button opens the window. Two reads of GameTime 2 s apart are
   equal, and `main+0x38A51` bit 0 is set (`tacli peek`).
   - **Clicks on the margin.** Left and right device clicks on the margin, over the selected
   ARMCK and over empty ground, leave the selection and its orders unchanged (`Check.orders`).
   - **Keys.** An arrow held 1 s leaves the eye unchanged (`main+0x1431F`/`+0x14323`). So does a
   device move held on the screen's last column. The Pause key leaves the game paused. The key
   ring's two indices stay equal throughout.
   - **Closing.** The Esc press keeps the window open; its release closes it. The bit is clear and
   GameTime runs.
   - **The X.** Reopen and press the X, releasing over a unit: the selection is unchanged.
   - **Level end.** Reopen and end the level with `scenarios/replay-end.json`: the dump says
   closed, and an injected key reaches the front end.
3. **`tools/wiki_regression.py <instance> recorded --launch`**, a live recorded skirmish, about
   3 minutes: the button sits 3 bar pixels left of the rewind button, and both open what they
   should.
4. **`tools/wiki_regression.py mp`**, a two-player network game started by the compatibility
   suite's launcher on its own DirectPlay port, about 8 minutes. Open the wiki on the host. Both
   peers' GameTime advances over 3 s. Margin clicks order nothing. After closing, neither log has
   a desync line.
5. **`tools/replay/tareplay.py landing`**, run by the runner: the `resource-*` groups.
6. **A capture of the open window** at 1920 × 1080 for the owner, and the place of the engine's
   `PAUSED` icon in single player (question 10).

**Review: high.** It writes engine state, adds a word that the engine's mouse thread reads, adds a
packet area and a part of the command record, and touches the input path.

### Landing 3 — the page

**Status: integrated with landing 4 on its worktree branch, not landed** (below, *The integration
of landings 3 and 4*). Track B, committed offline before it:
- the pre-step: `art/wiki/layout.json` holds `GEOM` (as `geom`), the shares, the strips, rows,
  paddings, the scrollbar, the controls' and the badge's numbers, the window's share and font,
  and `roles`, element by element. `wiki.js` sets each number as a custom property of the frame
  (`--w-` and its path) and `wiki.css` reads those. Every element's box, computed text style and
  painted-part canvas size, dumped headless at 1920 × 1080 and 3840 × 2160 in both skins, is
  byte-identical before and after the move. `WIKIPAK` packs `layout.json`.
- `tagpu_wikilay.c`/`.h`, pure: the state is `WIKIPAGE` (screen, bar, rewind, open, entry, demo,
  folds, three scrolls, playing, done, speed, try, `MOD_*`, the held control, the caption and the
  held keys); the faces are `WIKIFACES` (HUDART's faces by HUD style); the output is `HUDOPS` and a
  `WIKIFRAME` (each scroll as drawn and its limit, the stage rect, the window). It also lays out
  landing 2's window, title, close button and wiki button, with the keys `window`, `close` and
  `wiki`. The face lightness is `tagpu_wikilay_face_light`, set per part with
  `tagpu_wikilay_light`. The kit is read by `tagpu_hudlay_load_kit`, added to `tagpu_hudlay.c`.
- `tools/wiki/wikilaycheck.py` with `wikilaycheck.c`: ten cases over the five resolutions, stock,
  Escalation and TA Zero, both entries, folded and unfolded, the text scrolled (part way and to its end),
  captions and keys shown, a control held, 2×. Every part, control, disc, text line (start, width,
  baseline) and scroll limit agree within 0.07 CSS px, and every thumb Chrome paints to the
  pixel, in about 65 s.

**What the integration did for it**: `tagpu_wikistate.c` fills `WIKIFACES` from the wiki's
`HUDART`, and the art's loader sets each part's light from its decoded image (premultiplied) before
the publishing store; `tagpu_huddraw.c` honours `HOPF_INK`; the record carries `WIKIPAGE`'s fields
and `TAGPU_CMD_WIKI` the frame's hit map, scrolls, limits and scrollbars (`WIKIFRAME` gained each
panel's track and thumb). The control glyphs and the tree's marks are kit icons now (below). One
gap the check names stays open ([the note](ingame-wiki.html), *Gaps*): Chrome sets a face at a
quantized size.

**What the player sees.** The tree, with its folds, and each entry's demo list. The entry column:
- the title, the subtitle, and the red badge with "Turned off in this mod";
- the stage's bezel, grey until landing 4;
- the caption and keys strips, at fixed heights;
- the controls (play/pause, replay, 1×, 2×, Try it yourself), disabled with no demo;
- the text, scrolling.

Long names wrap. Row and button faces come from the kit, with their ink chosen by how light the
face is, and the text styles from `art/hud/layout.json`.

**A pre-step in the mockup, with no visual change.** The mockup's layout numbers move into
`art/wiki/layout.json`, which `wiki.css`, `wiki.js` and the DLL then all read:
- `GEOM`;
- the column shares 17 % / 20 %;
- the strip heights 27u and 19u, the 16u rows and their paddings;
- the text panel's 24 %;
- the 5u scrollbar;
- the text-role-to-style table `ROLE`.

`wikilaycheck.py`, run on the mockup before and after the move, proves nothing moved.

**Files:**
- **`src/tagpu_wikilay.c`**, the columns:
  - the tree's 7u indent per level;
  - greedy line breaking at spaces with the faces' metrics (`tagpu_hudtext_width`), and the
    browser's line rule from `tagpu_hudlay`;
  - the lock dot;
  - the panels' scrolling and thumbs;
  - the badge;
  - the face lightness, computed once at load from the decoded part, as `faceLight` does: an 8 × 8
    box average of the middle against a grey of 120;
  - the hot keys `fold:<node>`, `entry:<node>`, `demo:<i>`, `play`, `replay`, `x1`, `x2`, `try`,
    `scroll:tree`, `scroll:demos`, `scroll:text` and `stage`, given only to rows on screen, so 160
    hits are enough.
- **`src/tagpu_wiki.c`**: the selection, the folds (a bit per tree node; the loader caps the tree
  at 1024 nodes), three scroll offsets in units clamped to the limits the last drawn frame posted,
  playing, speed and try (state only, no simulation yet), the held control, and
  `tagpu_profiles_mod()` as the mod. The layout maps `MOD_RETAIL`, `MOD_ESC`, `MOD_MAYHEM`,
  `MOD_PROTA`, `MOD_TAZERO`, `MOD_TWILIGHT` and `MOD_UNKNOWN` to the data's `lockedIn` names.
  Selection, folds and scroll last for the session and are never saved.
- **`src/tagpu_wikipk.h`**: the record and the command part grow, adding the scroll limits.
- **`tools/wiki/wikilaycheck.py`** and **`wikilaycheck.c`**, after `tools/hudart/laycheck.py`. The
  mockup is opened headless, and every element's box, text line and baseline is paired with the C
  layout's.

**Who reads and writes what.** The game thread reads `tagpu_profiles_mod()`, which is Impure's own
state and no engine memory. No file names an engine address.

**The comments that must stand:**
- `tagpu_wikilay.c`: "each function is one of the mockup's rules, in its order"; the line rule; and
  that it is pure, with one call per frame on the render thread.
- `tagpu_wiki.c`: every scroll is clamped to the limits of the frame the player saw.

**Verification:**
- `python3 tools/wiki/wikilaycheck.py`, under 2 minutes. The cases: 1920 × 1080, 2560 × 1440,
  3840 × 2160, 3440 × 1440 and 1024 × 768; stock and Escalation; both entries; folded and unfolded;
  the text scrolled.
- One game run of `wiki_regression.py <instance> page --launch`, about 4 minutes: select the second
  entry, fold and unfold, scroll the text, compare the dump's state.
- Captures for the owner at 1920 × 1080 and 2560 × 1440, beside the mockup's.

**Review: medium.** No replay gate is expected; `--check` confirms.

### Landing 4 — the stage, and the demos playing

**Status: integrated with landing 3 on its worktree branch, not landed** (below, *The integration
of landings 3 and 4*). Track C, the stage offline, committed before it (`978ac43e`, `8ba9a6d0`,
`a8222dd0`), and where it settles what the text below left open:

- **The pre-steps are done.** `stage.js` draws the stage (`wiki.js` only calls it), in the face of
  `art/hud/layout.json`'s `toast` style (Oswald 700, no spacing, no case change, no shadow) with
  each line centred on its point as the HUD centres a line box, and a name longer than 1.5 times its
  box shrinks to fit (the HUD's rule) where the canvas used to squash it. The cursors are the kit's
  `cursor_arrow`, `cursor_move` and `cursor_build`, rasterised by `tools/wiki/cursors.sh` from
  `stage.js` `cursorShape` at 6 pixels a unit, each drawn centred on the pointer, so no hotspot
  needs a field in `kit.json`.
- **The copy is `tagpu_wikistage_copy`**, in the pure file beside the struct it fills, so
  `tagpu_wiki.c`'s fill calls it rather than writing its own; the record carries a
  `WIKISTAGEVIEW`. The bounds are the ones above, plus 256 vents; positions are floats except the
  cursor's, a double, because the cursor's icon box is snapped to a pixel. **It is 60 792 bytes**
  (i686), not about 40 KB: the map's 32 KB, the units, sites, queued and lines at their bounds, and
  the entry's 64 type names.
- **The ops.** A canvas arc is `HOP_DOT` with `HOPF_RING` (`HUDM_RING`, unsnapped): stroked, a
  ring; filled, a ring as deep as its radius. A `HOP_TEXT` op's opacity is its `rgba` low byte, for
  the queued sites' translucent labels; the HUD's own text ops hold it at `0xFF`. Every op clips to
  the stage's canvas. The art comes in as `WIKISTAGEART`: the `toast` style's index, the faces'
  measure, the three cursor parts and their sizes, and a log hook.
- **The overflow** is counted in `WIKISTAGEOPS.dropped`, logged once through the art's hook while
  `logged` stays set, and shown as a red edge round the stage in an op the budget keeps for it.
- **`stagecheck.mjs` passes:** all 340 frames of the 21 demos at each of three stages (the
  mockup's at 1920 × 1080 and 1024 × 768, and a small one with no grid where every label shrinks),
  1 852 765 ops paired call for op within 1/64 px, in about 10 s.

**What track C left for the integration**, now done (below), but for the budget:

- **`tagpu_huddraw_build` takes a `HUDOPS`**, whose arrays are `HUD_MAXOP` long: the draw now takes
  an op list (`tagpu_huddraw_build_list`, a pointer and a count).
- **A large map does not fit the budget.** Cells and fog are one op a cell each, as the canvas
  calls them: a 128 × 128 map's alone would take 32 768 ops. The data's map is 40 × 24, under
  3000 ops a frame with its grid (1816 on average over the check's frames); a square map past about
  70 cells a side with fog everywhere and its grid drops ops.
- **The cursor icons are in `art/wiki/kit/` as fitted.** Track D's fit rewrites that directory
  from `art/wiki-full/kit/`; the icons go there too (run `cursors.sh art/wiki-full/kit`), or the
  fit drops them.

#### The integration of landings 3 and 4

**Built 2026-10-25 on its worktree branch, not landed.** Against the two landings' lists:

- **One layout.** `tagpu_wikistate_present` lays the window out with `tagpu_wikilay_build`, the
  layout `wikilaycheck` pairs with the mockup, and builds the stage with `tagpu_wikistage_build`
  inside the bezel that layout placed, from the record's view. `tagpu_wikiframe.c`, which laid out
  the window, title, close button and wiki button a second time, is deleted, and its load-time
  check of the kit is `tagpu_wikilay_check` (the icons, the `toast` style, and the page built whole
  in its closed, open and every-demo states at 4:3, 16:9 and 32:9, *Renderer coverage*). The wiki's art loads
  the page's `WIKILAY` (`layout.json` and the kit, the kit carrying the HUD's styles, which
  `tagpu_wikilay_load` now takes as the HUD's `HUDLAY`).
- **The draw takes an op list.** `HUDOPLIST` is a pointer and a count for each of the ops, runs,
  text and groups; `tagpu_huddraw_build_list` draws one and checks every index an op carries
  against those counts, and `tagpu_huddraw_build` wraps it for the HUD's own `HUDOPS` unchanged
  (`laycheck.py` agrees). The wiki layer draws two lists, the page and then the stage: nothing of
  the page after the bezel overlaps the stage, so the paint order is the mockup's. A slot holds
  32 768 quads (8192 before), and one text op sets up to 2048 glyphs (`TAGPU_HUDTEXT_MAX`, 160
  before, which cut every line of the text panel short): the layout's own `MAXGLYPH`, tied to it
  at compile time.
- **The record.** `TAGPU_PK_WIKI` carries `WIKIPAGE`'s state and, last, the stage's
  `WIKISTAGEVIEW`; its area is `TAGPU_PK_WIKI_HEAD` bytes with no demo shown and the whole record
  with one, so the 60 KB view travels only while the window shows it. The render thread copies the
  view out of the packet and bounds every count and string before building from it.
- **The command record** carries, beside the hit map, each panel's scroll as drawn, its limit and
  its scrollbar's track and thumb in client pixels; the take bounds each number as data.
- **The controls**, on a left press against the last drawn frame: `fold:<node>` flips the node's
  bit; `entry:<node>` picks an entry other than the selected one, its first demo, playing;
  `demo:<i>` picks a demo afresh; `play` pauses or plays, and on an ended demo starts it again;
  `replay`; `x1`, `x2`. `try` shows pressed and does nothing (landing 5). The wheel scrolls the
  panel under the pointer 48 units a notch; a press on a thumb drags it by the drawn frame's ratio
  of scroll to travel, and a press on a track off its thumb pages by seven eighths of the panel.
  Every scroll is clamped to the limit the drawn frame posted. A notch or a page steps from the
  scroll asked, so several between two hit maps each count; a hit map that drew no wiki moves no
  scroll; a real move with the left button up ends a drag a real press began (its release can be
  lost outside the window). The hit map names the entry its frame drew, and `demo:`, `play`,
  `replay` and `try` count only when it is the entry selected now. After a close, a press of any
  button on a control of a hit map still drawn open (a double-click on X) is taken and owed its
  release, and does nothing.
- **The record's area when the slot cannot grow**: the whole record cut is placed as its head, the
  stage showing a message; a head cut too closes the window at the next publish's first fill, so
  the window is never open past one frame that cannot show it.
- **The simulation's lifetime and clock**: made at a selection, a replay and an open (the tree's
  first entry when none is chosen), freed at a change, a close and the level's end, all on the game
  thread; stepped inside the fill by `wiki.js` `advance` -- the `QueryPerformanceCounter` delta,
  capped at 0.25 s, times 30 times the speed, at most 8 ticks a frame -- and copied with
  `tagpu_wikistage_copy`, a truncation logged once a demo.
- **`tagpu_wiki.check`** plays every demo of every entry to its end in the fill and logs
  `wiki check: <entry>/<demo> <hash>`, the form `simparity.mjs --hash` prints, then a summary line.
- **The dump** adds the selection by id, the folds, the scrolls asked, drawn and their limits, the
  scrollbars, the demo's tick, `done`, its state hash, and the most one fill of the open page has
  cost the game thread (`cost_max_ms`, also logged at each close): timed from the record's start in
  `tagpu_wiki_fill` to its placing in the packet (`tagpu_wiki_carried`), so the demo's steps, the
  view's copy into the record and the record's 60 KB copy into the slot (`append_area`), and not
  the check or the dump.
- **The glyphs** (a commit of its own, the recommended fix pending the owner): play, pause, replay
  and the tree's marks are kit icons drawn from `tools/wiki/mockup/glyphs.js`, in the mockup and
  in the game; `wikilaycheck` pairs them as parts and now fails on any text set in the browser's
  fallback face.

MEASURED 2026-10-26 on the merged branch (the review fixes included), on Wine under a private
Xvfb: `wiki_regression.py` sp passed at 1920 × 1080 and at 3840 × 2160, its step (h) included,
and page, recorded and mp passed. `demos` passed at both sizes: all 21 `tagpu_wiki.check` hashes
equal `simparity.mjs --hash`, the 21 demos played to their ends in 12–13 ms on the game thread; a
demo ran 30.4 ticks a second at 1x and 58.9 at 2x at 1920 × 1080, 30.0 and 55.8 at 3840 × 2160;
the open page cost the game thread at most 0.063 ms in one frame over 2782 frames at 1920 × 1080
and 0.197 ms over 1285 at 3840 × 2160, against the plan's 0.5 ms, and 0.018 ms on the mp host.
The captures at both sizes, each entry with a demo paused mid-play beside the mockup at the same
state, agree but for the game's own top bar. `tareplay.py landing` passed its 34 test groups.

**What the player sees.** Selecting a demo plays it on the stage, as the mockup's whitebox:
- the cells, metal, vents and fog;
- units, the selection, construction and reclaim;
- the ghost, queued squares, order lines and a gesture's sites with their order numbers;
- the drag box and the cursor;
- the pace readout, and the spacing readout.

The caption and the keys strip follow the demo. Play/pause, replay, 1× and 2× work, and a demo
holds its last frame.

**Pre-steps in the mockup** (questions 4 and 5):
- `draw()` and `cursor()` move out of `wiki.js` into `tools/wiki/mockup/stage.js`, which runs under
  node as `sim.js` does;
- the stage's text moves to a HUD face;
- the three cursor shapes become kit icons, rasterised once from the mockup's own paths.

**Files:**
- **`src/tagpu_wikistage.c`/`.h`**: **pure.**
  - **Its input**: the record's stage part, the stage rect in units, and u.
  - **Its output**: `HUDOPS`, in the mockup's paint order:
    - cells as `HOP_SOLID`, with its `floor`/`ceil + 1` geometry;
    - grid lines while a cell spans 5 px or more;
    - vents as discs and rings;
    - fog;
    - dashes as `HOP_LINE` runs, by the canvas dash rule: the pattern runs on along a polyline
      and round a `strokeRect` from its top-left corner;
    - units, labels, selection brackets, sites, the ghost, the drag box, the cursor icon and the
      readouts.
  - **`tagpu_wikistage_fit(w, h, stage_w, stage_h, u, out)`**: the mockup's fit, with a pad of 6u.
    Both threads call this one function (landing 5).
  - **The wiki's op budget is its own**: `WIKI_MAXOP` 16384, about 0.9 MB on the render thread.
    An overflow is dropped, logged once, and visible.
- **`src/tagpu_huddraw.c`** and its shaders: `HUDM_RING`, a disc with an inner radius, for the
  vent's rings and the build cursor's circle. The SPIR-V is regenerated by `spirv-gen.py`.
- **`src/tagpu_wiki.c`**:
  - **The simulation's lifetime.** It owns the `WIKISIM`: made at a selection, a replay or an
    open, and freed at a change, a close and level end. All of that happens on the game thread.
  - **The clock** follows `wiki.js` `advance`: the `QueryPerformanceCounter` delta × 30 × speed,
    capped at 8 ticks a frame, stepping while the demo is not held.
  - **The copy.** The fill copies the view into the record's stage part, every array with its
    bound and a truncation bit:
    - the map at most 128 × 128 cells, metal as u8 and fog as u8;
    - units ≤ 128;
    - sites ≤ 256 (`PLACE_MAX_SITES`);
    - queued ≤ 256;
    - lines ≤ 512;
    - the caption ≤ 255 bytes;
    - labels ≤ 23.
  - **The trigger `tagpu_wiki.check`** plays every demo of every entry to its end, at once on the
    game thread, and logs `tagpu_wikisim_hash` for each.
- **`src/tagpu_wikipk.h`**: the stage part, at most about 40 KB, carried only while the window is
  open.
- **`tools/wiki/stagecheck.mjs`**. For every demo at every 30th tick:
  - `stage.js` draws `sim.js`'s view into a recording context;
  - a native build of `tagpu_wikistage.c` builds the same view's ops;
  - the two are paired call for op, within 1/64 px.

**Who reads and writes what.** The game thread steps the simulation, reading only `WIKIDATA`, and
writes the record. The render thread reads the record. No engine memory is touched.

**The comments that must stand:**
- **Where the fill steps**: it steps on the game thread, and the render thread gets values.
- **The copy**: why it is by value, and every bound.
- **The fit**: it is one function on both threads, so a click on the stage and the picture of the
  stage cannot disagree.

**Verification:**
- `node tools/wiki/stagecheck.mjs`, under 2 minutes.
- One game run of `wiki_regression.py <instance> demos --launch`, about 5 minutes:
  - touch `tagpu_wiki.check`: every demo's hash equals `simparity.mjs --hash`. This is the parity
    check of the code the DLL actually runs: i686, the game thread, SSE2;
  - play two demos at 1× and 2× and capture them for the owner;
  - log the most the wiki cost the game thread in one frame.
- The same cost logged in the `mp` run of landing 2's script, with a demo playing. The target is
  under 0.5 ms a frame.

**Review: medium.** A shader, the record's growth, and the game thread's cost.

### Landing 5 — Try it yourself

**What the player sees.** Try it yourself takes over the stage from the demo's scene, with the
build menu column beside it. The pointer, the buttons, Shift, Alt and Ctrl, and Alt+Z or Alt+X
drive the simulation as in the mockup. Back to demo returns.

**Files: `src/tagpu_wiki.c`.** In Try it:
- **The pointer.** Over the stage, it goes to the world through `tagpu_wikistage_fit` on the last
  drawn frame's stage rect (from the command record), clamped to the map, and becomes
  `input({move})`.
- **The buttons.** A left or right press on the stage gives a move and then a down; a release
  anywhere gives the up of a held button.
- **The keys.** Shift, Alt and Ctrl give down and up; Z and X with Alt give a down, an auto-repeat
  included.
- **Release all** on leaving Try it, on replay, on close and on focus loss, as `wiki.js` does.

**The build menu.** `tagpu_wikilay.c` lays out a 66u column: the selection's build options among
the entry's units, plus "none", in `face_chip`, with the keys `build:<type>` and `build:none`. A
press gives `input({build})`.

**The cursor.** The render thread hides it, with no cursor record, when the frame is in Try it
and the record's pointer lies inside that same frame's stage rect. Both facts are of one frame, so
there is no one-frame flicker. The stage draws the simulation's own cursor there.

**Who reads and writes what.** Game thread only. Nothing new crosses threads, beyond the record's
`try` flag and the hit map's stage rect.

**The comment that must stand.** The world point comes from the frame the player saw.

**Verification:** `wiki_regression.py <instance> try --launch`, about 4 minutes. It replays one
demo's inputs (`snap`, then `line`) as device input on the stage, inverting the fit from the
dump's stage rect. The final hash must equal `sim.js` fed the world inputs the dump logged. Then
captures.

**Review: medium.**

### Landing 6 — the stage in the game's graphics (scoped, not designed)

The mockup's `gfx=game` draws **sprites**: each unit rendered once from its 3DO at rest, facing
`0x8000`, flat-shaded by the `PALETTE.SHD` rows with player 0's frames. It also draws the deposit
and the vent at their GAF hotspots, a ground tile, and two cursors. The DLL would do the same,
baking the sprites once at the first open into extra part images. What it needs:

- **The cursors.** Already in the session cursor table, and in the UI atlas. Cheap.
- **The unit sprites.**
  - **The baker**: a C port of `tools/wiki/gfx.py`'s renderer, applying `tagpu_render3do.c`'s face
    rule.
  - **Its source**: the level's own model templates, which TA loads for every unit type
    [INFERRED]. That is engine memory read on the game thread, so it needs a publisher line in
    `thread-split.allow`, or a function in an existing publisher (`tagpu_packet_pub.c` already
    reads the same templates for `fill_pieces`). An owner decision.
  - **A mod without a unit** keeps that unit's whitebox.
- **The ground, the metal and the vent.** The mockup's are one lush map's (Crossing Two Rivers
  tile 13, `lushmetal3`, `lushvent01`), which the current level need not have loaded. Two ways,
  both owner decisions:
  - read them from the install's archives at the first open, a new archive reader;
  - draw the current level's own tiles and features, which changes the specification.
- **The cost**: a baker, a rasteriser and an asset source; high review; its own plan. **Recommended
  only after the whitebox wiki has shipped** and the owner has chosen the art direction
  (question 11).

### What runs in parallel

Once the core is in, four tracks touch no common file until they integrate:

| Track | Touches | Integrates as |
|---|---|---|
| A — the engine-facing shell | input hooks, polls, pause, packet hook, Vulkan wiki layer, cursor record | landing 2 |
| B — the page, offline | `art/wiki/layout.json`, `tagpu_wikilay.c`'s columns, `wikilaycheck` | landing 3, after A |
| C — the stage, offline (**committed**) | `stage.js`, `tagpu_wikistage.c`, `HUDM_RING`, `stagecheck` | landing 4, after A and B |
| D — the kit fitted | `art/wiki-full/`, `fit.py --set wiki` | inside landing 2 (A needs the button and frame parts) |

Landing 1b runs beside any of them. Landing 5 follows 4. Landing 6 waits for the owner.

**The merge surfaces** are `tagpu_wikipk.h` and `tagpu_wiki.c`, which landings 3 to 5 each extend.
Integrate them in order, and keep each track's pure file its own.

## Risks

1. **Wrapping parity.** Chrome breaks lines by UAX #14; the C layout breaks greedily after spaces
   and after a hyphen between a letter and anything but a digit (UAX #14's HY), which today's text
   needs: "right-click", "Shift-drag" and "Shift-click" are in both entries. `wikilaycheck`
   compares every line's text. A new entry with a slash or a dash may need the rule extended.
2. **The cursor record** is a new pipeline in `tagpu_vk_gui.c`. Drawn through `lay_viewport` it
   lands on the composite's pixels. A one-pixel offset from the copy beneath would show as a
   doubled edge in the margin, so landing 2's capture checks it.
3. **The real input path is not under test.** Every scripted test walks the injected path. The real
   path calls the same functions (`tagpu_wiki_message`), with the client point from `lParam` as the
   HUD does. A player's hardware click is shown only by the owner's hand-over or by the Windows
   test box, with the owner's permission. The same holds for the sprocket and the HUD today.
4. **The gate's length.** The `resource-*` groups are 17 games, each played several ways. If
   `tareplay.py landing` runs past the ten-minute budget, scoping it down would be a loosening, so
   ask; do not trim it.
5. **The game thread's cost in multiplayer.** At most 8 simulation steps a frame, inside the fill.
   It is measured in landing 4 against 0.5 ms; if it runs over, the cap of steps per frame is the
   lever, and it changes only how a demo catches up.
6. **Third-party plugin DLLs** that poll input through their own imports are outside the capture.
   Impure refuses TADR's executable. No setup in the compatibility suite acts on keys in play
   [INFERRED].
7. **Drift between the feature and the port.** Landing 1b closes it for the take-off rule. For the
   resource rules, `rulecheck.py` notices drift without closing it.
8. **The engine's `PAUSED` icon may lie under the window.** It is drawn at DrawGameScreen locals
   `[esp+0xAC]`/`[esp+0xB0]` (`0x46A110`), a place not established. The mockup's top-bar `PAUSED`
   is a stand-in. Landing 2's captures put it under the window, unseen (question 10).

## Questions for the owner

**Answered 2026-10-25:** the owner took the recommendation on questions 1, 2, 3 and 6: no wiki
on GDI; the pause bit written from `tagpu_menu.c`'s grown argument; the take-off rule shared
(landing 1b) and `tagpu_resource.c` not shared; the wiki available during a replay, pausing the
playback. The other questions proceed on their recommendations until the owner says otherwise.

Each comes with the recommended answer. Questions 2 and 3 are loosenings or changes to what was
asked; the rest are design choices the specification leaves open. None is one of the escalation
reasons: every verification above can run, and no fix depends on timing.

1. **The wiki on GDI.** *Recommended: no button and no wiki,* as for the replay HUD and the rewind
   button. GDI is the original game's reference, and the kit's RGBA nine-slices have no 8-bit path.
2. **Where the pause bit is written: a loosening.** Either `tagpu_menu.c`'s `thread-split.allow`
   argument grows by a read of `main+0x2A44` bit 0 and a read-modify-write of `main+0x38A51` bit 0,
   or `tagpu_wiki.c` gets a new `publisher presentation` line. *Recommended: grow `tagpu_menu.c`'s
   argument.* That file already owns Impure's top-bar button and the HUD's engine questions, and it
   is already a source of the test groups this landing runs. The wiki's own files then name no
   engine address.
3. **Shared rule code.** The note asks the DLL to call the real feature's rules "where it can".
   *Recommended:*
   - `tagpu_placegeo.c`, as the core already does;
   - the take-off rule factored into it and shared (landing 1b: one test group, a high review);
   - not `tagpu_resource.c`, whose rules are engine reads throughout and would mean rewriting the
     shipped feature. `rulecheck.py` catches its drift instead.
4. **The stage's font.** The mockup draws the stage's labels in DejaVu Sans, which the DLL does not
   carry. *Recommended: change the mockup to the HUD's faces,* rather than embed a fourth font.
5. **The stage's cursors.** The mockup draws three vector shapes, and the DLL's ops have no polygon
   fill. *Recommended: three kit icons rasterised from the mockup's own paths,* drawn by both.
6. **The wiki during a replay.** *Recommended: available, in the rewind slot, pausing the playback*
   as it pauses single player. The play button shows the pause; the playback is restored on close.
7. **Opening over the engine's menus** (the in-game menu, chat, Leave's question). *Recommended:
   allowed.* The wiki takes the input while it is open, and the menu is as it was on close.
8. **The badge's source.** *Recommended: the data's `lockedIn`, as specified.* It matches every
   profile Impure ships (`resource-snapping.md`, *Scope and defaults*). A profile a player has
   edited to permit Quick Build in a locked mod is the gap; reading the profile's permission instead
   would change the data format.
9. **The window's system keys.** Alt+Enter and Alt+F4 do nothing while the window is open.
   *Recommended: accept.* Letting `WM_SYSKEYDOWN` through would hand the engine's translator a key
   at `0x4B5DC3`.
10. **The paused mark in single player.** If landing 2's capture shows the engine's `PAUSED` icon
    under the window: *recommended,* a "Paused" chip in the window's title groove, added to the
    mockup first. If the icon shows, nothing.
11. **Landing 6's direction**: whether the stage gets the game's graphics at all, and if so whether
    the ground and resources are the mockup's lush ones, read from the archives, or the current
    level's. *Recommended: decide after the whitebox wiki ships.*

## What this plan does not know yet

- **Whether the fitted kit holds up at 3440 × 1440 and 1024 × 768.** The xtile repeats and the
  window part's slices of 231 and 416 units at small sizes. `wikilaycheck`'s cases include both.
- **The game thread's cost for the heaviest demo.** `bestfit`, at 721 ticks, runs `quick_choice`
  and the deposit flood fill on every hover.
- **The model templates** [INFERRED]: whether every unit type's template is resident in every level
  (landing 6).
