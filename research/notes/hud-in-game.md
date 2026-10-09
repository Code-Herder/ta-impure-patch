# The replay HUD in game

## Status

**Landing 1 is built: the replay HUD draws in game, behind a lever, with no input.** The replay
HUD of [The replay HUD's layout](hud-layout.md) — the column of team headers and player rows at
the top right, and the dock at the bottom with its transport buttons, the timeline and its
markers, and the clock — is drawn by a Vulkan pass of our own while a recording plays and
`tagpu_replayhud.on` is in the game folder. It reads the same files as the mockup, `art/hud/`,
and implements the same rules, so the two stay 1:1. Under the lever the watching dock and the
player cards of [S5](tadr-port/demo-recorder-s5-ui.md) stand aside; nothing yet takes their
input, so a playing watched with the lever on has no seek, pause, speed or Leave on screen
(*The landings*, *Gaps*).

**Verified on 2026-10-08** (MEASURED, the reference setup: a private 3840 × 2160 Xvfb with the
RTX presenting, a four-seat 2v2 skirmish recording on Two Continents). At replay HUD 150 with the
game's HUD at 150, and at the store's 100 with the game's HUD at Auto (450 %), the game's
capture and the mockup fed the game's own dump match: the column, the dock, the team headers, the
lost-commander skull, a defeated player's dimmed row, and the timeline's death and defeat
markers. The best vertical and horizontal shift of the game's crop against the mockup's is 0 px
for the column, for the dock and for every text crop tested. How the check is run:
*Checking it against the mockup*.

The owner's decisions (2026-10-08):

- **A pass of our own, not a UI library**, with stb_truetype for the fonts. The reasons are in
  [The replay HUD's layout](hud-layout.md), *Status*.
- **The HUD keeps its own scale and fits the space the engine's frame leaves.** The replay HUD's
  scale sets the unit; the engine's side panel and bars, at the game's HUD scale, set the
  anchors (*Units and the screen* there).

## Turning it on

- **`tagpu_replayhud.on`** in the game folder (`tacli arm <i> replayhud.on`). It is read once a
  process, the first time the game thread's packet fill or the dock's gate asks
  (`tagpu_hudstate_on`, `tagpu_hudstate.c`), and latched with an `InterlockedCompareExchange`
  because either can ask first. Arm it before the launch.
- **`scale=N` in the file**, N one of 75, 100, 125 and 150, is the HUD's own scale and wins over
  the settings store's, as `tagpu_hud.on` carries the game's HUD scale. An empty file, or any
  other content, turns the HUD on at the store's scale.
- **The store's key is `replayhud`** in `impure.cfg` (`TS_REPLAYHUD`, `inc/tagpu_settings.h`):
  100 by default, or 75, 125 or 150 ([Renderers](renderers.md), *the defaults*). No settings row
  writes it yet; the HUD's scale button will (landing 2).
- **One HUD unit is the viewport height / 720 × the replay HUD's scale**
  (`tagpu_hudstate_present`), whatever the game's HUD scale. The engine's frame — the side
  panel's width and the bars' height at the game's HUD scale, from `tagpu_hud_live` — gives the
  anchors, and the 9-slice and grid-sliced parts stretch to fill what is left.

## How it is built

| File | What it holds | Thread |
|---|---|---|
| `tagpu_hudstate.c`/`.h` | the lever; the packet's HUD record filled; the record turned into the layout's state, the teams, the names and the dump | game (fill), render (present) |
| `tagpu_hudlay.c`/`.h` | `hud.js`'s placement and builders in C, over `layout.json` and `kit.json`; the text baseline; a part op as quads; the hit-test (no caller yet) | either: a pure function |
| `tagpu_huddraw.c`/`.h` | the ops as instances; the glyph atlas; the shader pair | render |
| `tagpu_hudart.c`/`.h` | the loader: the `HUDPAK` resource, the parts decoded, the faces opened | its own, then immutable |
| `tagpu_vk_hud.c`/`.h` | the pass: the images, the uploads, one pipeline, a draw per run of instances | render |
| `tagpu_json.c`/`.h` | the JSON parser the layout reads its two files with | either |
| `tagpu_packet.h`, `tagpu_packet_pub.c`, `tagpu_packet.c` | `TAGPU_PK_HUD`, its area `hud_off`/`hud_len`, its fill, and `frame_valid`'s bounds on `nseat` and `nmark` | game → render |
| `tools/hudpak.py`, `res.rc`, the Makefile | `art/hud/` packed into `inc/tagpu_hudpak.bin` (gitignored), embedded as the `RCDATA` resource `HUDPAK` | build |

### Where it is drawn

- **Its own pass, after the world and before the engine's UI.** `tagpu_vk.c` calls
  `tagpu_vk_hud_prepare` just before `tagpu_vk_gui_prepare`, and `tagpu_vk_hud_record` just
  before `tagpu_vk_gui_record`, after the world target's composite.
- **The HUD covers the world and nothing else.** The engine's dialogs, its messages and its menu
  draw over it, and so does the cursor, which is the last quad of the UI pass's sharp layer.
- **It is alpha-blended at the device's resolution**, premultiplied (`ONE`,
  `ONE_MINUS_SRC_ALPHA`): the parts' straight alpha is premultiplied once, at load, so filtering
  and mipmaps cannot darken an edge.
- **It blends in the swapchain's encoded values**, `B8G8R8A8_UNORM`, as the browser blends, so
  a translucent part composes in the game as it does in the mockup.
- **It claims no A/B frame.** The census counts it among the passes that drew (`hud=N` on the
  `vk: census` line) and not among those that claimed, so an A/B capture in a playing with the
  HUD drawn stands down rather than comparing the wrong frame.
- **The render thread builds its state in the overlay frame.** `tagpu_overlay.c` calls
  `tagpu_hudstate_present` after `tagpu_gui_present`, which turns the packet's record into the
  layout's state and stamps it with the frame's `frame_counter`; the pass takes it through
  `tagpu_hudstate_frame`, which refuses any frame but its own (`tagpu_vk_pass.h`, `frame`). A
  frame whose overlay returned before `present` — a level teardown, `tagpu_overlay.off` — draws
  no HUD rather than the last one (MEASURED 2026-10-08: under `tagpu_overlay.off` the frame is
  black, and the HUD is back unchanged when the file goes).
- **Teardown.** `tagpu_vk_hud_down` runs in `vk_down` and in `passes_down`, behind the seam's
  `vkDeviceWaitIdle`, and leaves the pass UNBUILT so that it builds again on the next frame
  that has a HUD.

### The art and the layout in the DLL

- **One archive, embedded as a resource.** `tools/hudpak.py` packs `art/hud/` as it is —
  `layout.json`, `kit.json`, `parts/*.png`, `fonts/*.ttf` and the fonts' licence texts — behind
  a directory, into `inc/tagpu_hudpak.bin` (7.8 MB with the 44 parts of 2026-10-08), which
  `res.rc` embeds. Every offset is checked against the resource's size before it is read.
- **The JSON is read at run time, not converted.** `tagpu_json.c` reads the same text the mockup
  reads, and `tagpu_hudlay_load` resolves it into structs once. A field missing or of the wrong
  type stops the load, and the log names it (`hud: NOT loaded - …`). The load also runs every
  rule once on a state that takes every branch, so a layout that loads cannot fail to build.
- **Decoding is off the frame.** The first frame that has a HUD to draw starts the loader
  (`tagpu_hudart_start`), a thread of its own that decodes the parts, premultiplies them and
  opens the faces, then publishes the result with one interlocked store; the HUD draws nothing
  until then. MEASURED 2026-10-08: `hud: loaded 44 parts (5.7 Mpx) and 3 faces in ~230 ms`.
- **The decoded parts stay for the session**, 22.9 MB of RGBA. Every teardown and resize brings
  the pass down, and its images are uploaded again from these. Nothing writes or frees the
  loaded HUD afterwards, which is why any thread may read it without a lock.
- **Each part is its own image, with mipmaps.** The parts are painted at 3 to 26 pixels a unit,
  mostly about 9, and drawn at 1.1 (1080p, 75 %) to 4.5 (4K, 150 %), so most are minified 2 to 8
  times. One descriptor set per part is bound per run of quads.

### The uploads

- **The parts go once per build, across frames.** Their level 0 totals 22.9 MB against a slot's
  2 MiB staging cap (`TAGPU_VK_STAGE_CAP`, `tagpu_vk_stage.h`), so they take several frames:
  each frame resumes where the last stopped (`s_nsent`), a banded part uses none of a frame's
  room, and the rest go as many as fit it.
- **A part that does not fit what is left of this frame's buffer waits** to be the next frame's
  first copy. That copy goes in the frame's own command buffer when it fits the whole buffer,
  and banded when it does not. The banded ones are the four parts over 2 MiB: `win_one`,
  `foot_one`, `tp_one` and `rail_one`, sized from their PNGs. A pass's first upload after it is
  built is the event the banded path is for (`tagpu_vk_stage.h`).
- **The mip chains are made on the GPU.** After a part's level 0 lands, `blit_chain` blits each
  level down from the one above with `vkCmdBlitImage` and a linear filter, in the frame's command
  buffer. The parts are premultiplied, so a linear average is the right one.
- **The HUD draws once every part is on the device** and the atlas has a generation there.
- **The glyph atlas** is one 2048 × 2048 R8 image. It goes whole once after a build, as the
  frame's first copy, 4 MiB, banded. After that only the rows that changed go, in the frame's own
  command buffer, as many as the slot's buffer has room for, the rest on the next frames; nothing
  waits on the device in play. That holds for a new generation too: when a glyph does not fit,
  the atlas is emptied to zeros and refilled, and every glyph it then holds lies, with the empty
  texel round it, inside the changed rows, so the older generation's rows the device keeps are
  never sampled (`tagpu_huddraw.h`).
- **The text draws only from an atlas the device holds.** While changed rows wait for room, or in
  a frame with no staging, the frame's glyph instances are made transparent (`hide_glyphs`); the
  fills and parts in the same runs still draw. In a normal frame it changes nothing (MEASURED:
  the 150/150 capture is unchanged).
- **A failed upload refuses the pass** (`ST_REFUSED`): a submit or a fence the device failed,
  `-1` from the stage. The pass stops drawing at once and destroys nothing mid-frame,
  where other slots' submits still name its objects. `tagpu_vk_hud_down` gives them back behind
  the seam's `vkDeviceWaitIdle` on the next `vk_down` or `passes_down`.
- **A HUD that cannot draw gives the dock back.** Its art failing to load, its pass failing to
  build, the device failing an upload, or its layout failing to build: each calls
  `tagpu_hudstate_refuse` on the render thread, an interlocked store `tagpu_hudstate_on` reads on
  the game thread. From then on, for the session, the lever counts as off: no record is filled,
  and the watching dock and the cards open (`hud: the replay HUD cannot draw (…) - the dock and the
  cards come back`). Not run: none of the four can be brought about without breaking the build.

### The rules in C

- **`tagpu_hudlay.c` is `hud.js`'s placement and builders, in C.** It takes the screen, the
  engine's frame and the HUD's state, and returns a list of ops: a part, a solid, a dot, a text
  or a clickable, each with its rectangle in HUD units and its clip. It reads no engine state and
  no global, so the game thread and the render thread can call it on the same inputs and get the
  same rectangles.
- **The render thread calls it to draw.** `tagpu_hudlay_hit` is there for the game thread to
  hit-test a click against the same rectangles; nothing calls it yet (landing 2).
- **Grid-sliced parts are drawn cell by cell** (`tagpu_hudlay_expand`). A part expands into at
  most `HUD_MAXQUAD` = (2·`HUD_MAXBAND` + 1)² quads (`tagpu_hudlay.h`), the bound the loader
  enforces on each part's bands. The mockup composes a grid part into one image instead; at a
  cell's edge the two can differ by a pixel's filtering, and nowhere else.
- **`tools/hudart/laycheck.py`** compares this file's rectangles and baselines with the mockup's
  ([The replay HUD's layout](hud-layout.md), *Text*).

### The text

- **stb_truetype**, vendored as `inc/stb_truetype.h`.
- **The faces are committed cut down** (`tools/hudart/fonts.py`, `art/hud/fonts/`) and ship
  inside the archive with their OFL texts: Oswald 500 and 700 and Barlow Condensed 600.
- **No sigma.** Neither family carries Greek, so the team header's unit sum is the bare number,
  in `hud.js` and in `tagpu_hudlay.c` alike.
- **The players' names** are the engine's 8-bit text read as Latin-1 (`latin1_utf8`,
  `tagpu_hudstate.c`): a control code — below 0x20, or 0x80..0x9F — becomes `?`, and the soft
  hyphen, which Oswald lacks, becomes `-`.
- **Glyphs are rasterised at the exact pixel size each style needs** into the atlas, keyed by
  face, size in 1/64 px, glyph and the pen's fraction in quarters. The baseline is Chrome's and
  painted on a whole pixel; the pen keeps its fraction (*Text* in the layout page). Upper case,
  letter spacing, the drop shadow and the four-copy outline are done as the mockup does them.

### The data

- **The game thread publishes the HUD's state in the frame packet** as one `TAGPU_PK_HUD` record
  in the area `hud_off`/`hud_len`, present only while a recording plays and the HUD is on
  (`tagpu_hudstate_fill`, from `fill_frame`). It carries the clock (`now`, `first`, `last`), the
  replay's speed and pause, the HUD's scale, up to ten seats and up to 320 markers.
  `frame_valid` checks the length and bounds `nseat` and `nmark` by the record's arrays before
  the render thread loops over them. A record that does not fit the slot sets
  `TAGPU_PK_TRUNC_HUD` (`0x800000`), and that frame draws no HUD.
- **Each seat comes from the replay's image of its player record**, `W->player[seat]`, through
  `tagpu_replay_players` (`tagpu_replay.c`): the name, metal and energy stored, income and
  spending, the two storage maxima, the units alive, the commander-loss counter and the
  alliance row (the offsets are the [engine map](exe-reverse-engineering.md)'s player-record table). The colour is the
  cards' rule, `tagpu_cards_colour`, resolved through the presented palette.
- **"Lost"** is a commander-loss counter above 0 (the skull); **"out"** is no unit alive (the
  dimmed row).
- **The markers** are the watching dock's: its marker walker still runs under the lever and feeds
  the HUD through `tagpu_dock_marks` — the commanders' deaths and the defeats as far as the walker
  has read the recording, and the bookmarks. A marker outside the recording's span, or for a seat
  the HUD does not show, is dropped.
- **The teams are the mutual alliances**, joined transitively with union-find
  (`teams`, `tagpu_hudstate.c`): two seats are one team when each one's alliance row marks the
  other. When every seat is alone the HUD shows one team with no headers, as a free-for-all.
  In a skirmish the alliances are the SKIRMISH screen's: the seat fill `0x47A760` marks allied
  every human or AI seat that shares an alliance group other than 5 (the store at `0x47A890`;
  [engine map](exe-reverse-engineering.md), *The seat fill*). The group is saved in the registry as
  `Skirmish\Player<N>AllyGroup` ([command-line options](cmdline-options.md)) [INFERRED: the
  registry value reaching the settings entry's `+8` is not traced].
- **The rates** — a row's income and spending — read one decimal below 100 in magnitude and whole
  numbers from 100, rounded half up as JavaScript's `Math.round` rounds (`rate` and `js_round`,
  `tagpu_hudlay.c`; `rate`, `hud.js`). A resource bar whose storage is 0 is empty.
- **No engine state is written**, and the HUD adds no byte patch.

## Checking it against the mockup

- **Touch `tagpu_replayhud.dump` in the game folder.** The render thread looks for it every 30
  frames (`tagpu_hudstate_present`), deletes it, and writes `tagpu_replayhud.json` through a
  `.tmp` file and `MoveFileEx`: the state in the mockup's own shape and in HUD units, with the
  device's size, the pixels a unit is (`px`) and the engine frame's scale (`q8`). The log says
  `hud: state dumped to tagpu_replayhud.json: …`.
- **The mockup draws exactly that state** given `?state=<url of the json>`
  (`tools/hudart/mockup/index.html`). It loads the shipped faces from `art/hud/fonts/` with
  `@font-face`, so a glyph the faces lack is missing there as it is in game.
- **The check:** capture the game (`DISPLAY=:94 import -window root g.png` on the private Xvfb),
  capture the mockup fed the game's dump
  (`tools/hudart/capture.sh "<mockup url>?state=<json url>" m.png`), and compare the two at native
  4K, region by region, for the best shift.
- **Where the dock's markers come from in a test:** a recording in which a commander dies. In a
  live skirmish, `ctrl+c` selects the commander and `ctrl+d` self-destructs it; the run above
  logged `dock: markers: 1 commander deaths at 1677 (seat 0), 1 defeats at 2906 (seat 0)`. The
  walker (`tagpu_dock.c`) marks a death where a seat's commander-loss counter `+0x106` rises and a
  defeat where its live count `+0x144` falls to 0.

## The landings

1. **The HUD draws, behind a lever** — built. The pass, the archive, the parser, the rules in C
   and the text, drawing the dock and the column from the replay's data, with no input; under the
   lever the old dock and cards do not open. Checked as *Status* says.
2. **The HUD replaces the dock and the cards.** Input, hover, the commands (seek, pause, speed,
   bookmark, Leave, the sight request), the scroll and the scale button; the lever goes, and the
   old dock's screen and drawing are deleted. Clicks are to be consumed in the shield's chain, as
   the dock's are (`tagpu_shield.c`); hover is not consumed. Whether the GDI renderer keeps a HUD
   is decided here.
3. **The stats window.** It needs a per-second stats track the recorder does not write yet, so it
   follows a recorder landing.

Each landing is reviewed at `high`: it adds a pass and a hand-over between the game thread and
the render thread.

## Gaps

- **No input.** Under the lever nothing on screen seeks, pauses, changes the speed or leaves,
  and the HUD's buttons do nothing; `tacli replay seek` and the trigger's `seek` still work.
  Landing 2.
- **The stats window is never drawn.** The state's toggles are all off and nothing can turn
  them on; its data needs the recorder track of landing 3.
- **A bookmark marker is drawn but untested in game**: without input there is no way to add one
  under the lever.
- **Vulkan only.** The packet is the HUD's only way to the screen, and only the Vulkan lane reads
  it, so `tagpu_hudstate_on` answers yes only while `g_ddraw.renderer` is `vk_render_main`. On GDI,
  or once the Vulkan lane has handed the session to it, the lever does nothing: the watching dock
  and the cards open as they do without it (MEASURED 2026-10-08 under `tagpu_gdi.on`: `DOCK.GUI`
  over `ARMMAIN2`, no HUD pass). Whether GDI gets the HUD is landing 2's question.
- **A one-pixel Oswald residual.** At 26 of 7554 sizes swept, Chrome rounds Oswald's ascent or
  descent differently from the rule the DLL follows, and the line sits a pixel off the mockup's
  ([The replay HUD's layout](hud-layout.md), *Text*).

## Open

- **The HUD font** is not decided; Oswald and Barlow Condensed are the mockup's.
- **Address space.** The decoded parts stay for the session, 22.9 MB of a 32-bit process, and
  the archive stays mapped with the DLL, 7.8 MB.
