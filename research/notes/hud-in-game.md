# The replay HUD in game

## Status

**Landings 1 to 3 are built: the replay HUD is a replay's controls on Vulkan.** The replay
HUD of [The replay HUD's layout](hud-layout.md) — the column of team headers and player rows at
the top right with its foot plate of buttons, the dock at the bottom with its transport
buttons, the timeline and its markers, and the clock, and the stats window with its chart of
each player's or team's economy and army over the recording — is drawn by a Vulkan pass of our
own on every level that plays a recording, and it takes the player's clicks, hover, wheel and
drag (*Input*). It reads the same files as the mockup, `art/hud/`, and implements the same
rules, so the two stay 1:1. It replaces the watching dock and the player cards of
[S5](tadr-port/demo-recorder-s5-ui.md), which are deleted; on the GDI renderer a replay has no
on-screen controls (*When it draws*).

**The live rewind is built and run in game** (*Live rewind*): in a live recorded skirmish the HUD
draws the mockup's live mode instead — a rewind button beside the Impure Config sprocket in the
top bar and, while a rewind is open, its strip under the bar — and drives the rewind through its
own verbs. Its layout agrees with the mockup's (laycheck). MEASURED 2026-10-23 by the runner's
games, on the reference setup's private Xvfb:

- **at 8056ef6a**: U's 36 checks with the HUD cycle of that commit (open, rew, Back to now),
  every comparison EQUAL; the W control, W's 10
  checks (`tareplay.py run stock-limits --cover`); the button and the strip placed as the mockup
  places them at 1080 with the HUD at 100 and at 4K with the HUD at 150; the cursor over the
  button; the PAUSED rule; GDI unchanged;
- **at b590bfd4**: U and U-E 40 of 40 (U-E armed with its toast, forked, the eviction said, the
  fork EQUAL to its recording); the strip whole over nine message lines; the cursor over the
  strip; the toast held while armed;
- **at 54adfac8**: U and U-E 40 of 40 again (`tareplay.py rewind battle --cover`); the captures
  against the mockup with message lines on screen, the strip open and closed, and the armed toast
  with its eviction; a 250-frame recording of the Config panel opened and closed over the open
  strip, in which no frame shows the strip over the panel;
- **at c7bdb170**: U and U-E 45 of 45 with the HUD cycle *The tests* describe
  (`tareplay.py rewind battle --cover`); `tareplay.py run stock-limits --cover` 132 passed, none
  failed; the captures with message lines, the Config panel and the toast; and a world A/B in a
  recorded skirmish captures with the HUD in the frame, and stands down only while the UI pass
  is on too, as it always has.

What is still owed is in *Gaps*.

The word **dock** on this page and in `layout.json` is the HUD's own bottom part, the transport
plate and the timeline housing; the engine GUI screen of the same name (`DOCK.GUI`,
`tagpu_dock.c`) no longer exists.

**Layout verified on 2026-10-08** (MEASURED, the reference setup: a private 3840 × 2160 Xvfb with the
RTX presenting, a four-seat 2v2 skirmish recording on Two Continents). At replay HUD 150 with the
game's HUD at 150, and at the store's 100 with the game's HUD at Auto (450 %), the game's
capture and the mockup fed the game's own dump match: the column, the dock, the team headers, the
lost-commander skull, a defeated player's dimmed row, and the timeline's death and defeat
markers. The best vertical and horizontal shift of the game's crop against the mockup's is 0 px
for the column, for the dock and for every text crop tested. How the check is run:
*Checking it against the mockup*.

**Input verified on 2026-10-09** (MEASURED, the reference setup: a private 3840 × 2160 Xvfb, the
game at 1920 × 1080 on Vulkan, about 5 frames a second on that display; every control clicked in
device space where the dump said it was drawn, through `tacli hud`):

- pause and resume; the speed through every preset and the wrap from 8× to ½×; ±10 s; a bookmark
  added and removed, with `replays\index.dat` changed each time; the scale button through 100,
  125, 150, 75 and back to 100, written to `impure.cfg`; a team folded and unfolded;
- the four switches, each logging its line (`replay: overlay cursors off` … `replay: chat
  hidden`);
- hover showing a row's Jump, Sight and Follow; Sight on seat 2, then Sight again (record 10, the
  full map); Follow, then Follow again (`follow ended: free cam`); Jump; on a seat whose camera
  was not recorded, the toast "Camera was not recorded" (seen in a capture);
- a right click on a control taken with no action; the wheel over the column taken (no zoom line)
  and over the world zooming; a ten-seat, five-team recording at the replay HUD's 150 % scrolled from 0 to 4 by
  the arrows and by the wheel;
- the timeline scrubbed from 20 % to 60 %: one seek a frame, the last at 20671 against 20665
  computed, a pixel of that track being 14 ticks; play at the recording's end starting it over;
- Leave then No, from playing and from paused, each putting the pause back as it was; Leave then
  Yes, back to the Replays browser; a second Watch starting fresh (every switch on, nothing
  folded);
- the modal test: with ARMOPT open (Tab), a click on the HUD's bookmark reached the engine — no
  bookmark was written — and the engine closed ARMOPT, as it closes it for any click outside it;
- the walker's markers on the `hud-2v2-selfd` recording: a commander death at 1677 and a defeat
  at 2906, both seat 0; on timeline 4 of the fork fixture's `forks.imprec` (forked from the
  original at 1501), 2130 iterations walked to the fork's end, against the original's 1980, and
  nine defeats at 1952;
- the render-options panel open over the column: a device click on its FPS row, over the
  column's plate, reached the panel (the counter came on) and the HUD took nothing;
- Leave's question open: Tab opens nothing over it (the question stays on top, the pause held);
  a click on the world closes it, as one closes ARMOPT, and that counts as No — the pause came
  back and the playback went on; No from playing and from paused, each put back as it was;
- under `tagpu_gdi.on`: the playback runs, with no HUD and no marker walk.

Three faults were found that way and fixed before the landing: the four switches matched no
name (`IS()` in `act` compared through `sizeof` of a pointer, and now takes literals only),
several wheel notches in one frame moved the column one item, and the play button at the
recording's end showed pause.

**The stats window verified on 2026-10-09** (MEASURED, the reference setup: the private
3840 × 2160 Xvfb, the HUD at 150 %, the battle recording — a four-seat 2v2 on Two Continents,
1148 s — and the `hud-2v2-selfd` recording, 102 s, whose seat 0 loses its commander). The game's
capture against the mockup fed the game's own dump, at shift 0, 0, as the share of pixels
differing by more than 48 levels:

- army value at 0:12 on the battle, by player: the title 0.96 %, the chips 2.41 %, the chart
  0.47 %, the legend 0.54 %, the foot 0.27 %;
- `hud-2v2-selfd` at 1:25, the window dragged, the death cross at 0:56: 0.92 % (a mean
  difference of 2.31 levels);
- metal income on the battle, by team: 0.87 %.

The walker logged `383 stats samples 90 GameTime apart, 279 unit types valued` on the battle
(34 441 iterations in 2 371 ms) and `103 stats samples 30 GameTime apart` on `hud-2v2-selfd`. A
paused replay showed no `PAUSED` icon (`replay: the pause icon's test 0x46A107 in`); without
the stub, the engine drew it in the middle of the world, over the window. Leave → Replays →
Watch started the next replay with the window closed; opened again, it kept metal income, the
teams split and its dragged place, 83.3, 100.4.

The owner's decisions:

- **A pass of our own, not a UI library**, with stb_truetype for the fonts (2026-10-08). The
  reasons are in [The replay HUD's layout](hud-layout.md), *Status*.
- **The HUD keeps its own scale and fits the space the engine's frame leaves** (2026-10-08). The
  replay HUD's scale sets the unit; the engine's side panel and bars, at the game's HUD scale, set
  the anchors (*Units and the screen* there).
- **The HUD replaces the dock and the cards, which go whole** (2026-10-08); **GDI gets no replay
  controls** — "GDI gets nothing, this is just vulkan" (2026-10-09).
- **Sight and Follow are drawn pressed on the player they are set to, and a click on the pressed
  one undoes it** — the full map, the free camera. That replaces the cards' Full map and Free cam
  (2026-10-08).
- **The foot plate's five buttons:** game stats, which opens and closes the stats window, then
  Cursors, Clicks, Selections and Chat, the four switches of the dock's old Overlays menu. The
  map button and the capture button are removed (2026-10-08).
- **The army value is every finished unit that can move, at its type's metal cost, the commander
  excluded** (2026-10-09).
- **The stats window keeps its measure, its split and its place for the session**, never in
  `impure.cfg`, and is closed when each replay starts (2026-10-09).
- **A replay with the replay HUD shows no `PAUSED` icon**: the HUD's play button carries the
  pause, as the mockup shows (2026-10-09). GDI, with no HUD, keeps the engine's icon.
- Everything else follows the mockup, `tools/hudart/mockup/`, which is the HUD's specification.

## When it draws

- **On every level that plays a recording, and every live recorded skirmish, on the Vulkan
  lane.** There is no lever: `tagpu_hudstate_on` (`tagpu_hudstate.c`) answers yes while
  `g_ddraw.renderer` is `vk_render_main` and the pass has not refused (`tagpu_hudstate_refuse`,
  below), and `tagpu_hudstate_mode` says what the HUD shows. **The live HUD** while a rewind owns
  the level or one could open (`tagpu_replay_rewind_state`: its state is not idle, or `available`
  — a live, recorded, non-network skirmish whose recorder is healthy); **else the replay HUD**
  while the playback is live (`tagpu_replay_live`) and its span is known; else nothing. The
  rewind is asked first because a rewind into the past plays the recording as puppets, where
  `tagpu_replay_live` is true. `tagpu_hudstate_fill` fills the packet's record in either mode, its
  `mode` saying which.
- **On GDI, or once the Vulkan lane has handed the session to it, a replay has no on-screen
  controls**: no record is filled, the marker walker never starts (it starts from the fill), so
  there is no stats window either, the engine's `PAUSED` icon shows while paused, and the
  engine's own menu leaves (Tab, Exit). `tacli replay seek`, the trigger's commands and the
  keyboard still work.
- **The HUD's scale** is `tagpu_hudinput_scale` (`tagpu_hudinput.c`): the scale button's last
  choice in this process, else the settings store's `replayhud` in `impure.cfg` (`TS_REPLAYHUD`,
  `inc/tagpu_settings.h`) when it is 75, 100, 125 or 150, else 100. The button cycles 75, 100,
  125, 150 and records each choice in the store (`tagpu_settings_set`), which the render thread
  writes to `impure.cfg` at its next flush ([Renderers](renderers.md), *the defaults*).
- **One HUD unit is the viewport height / 720 × the replay HUD's scale**
  (`tagpu_hudstate_present`), whatever the game's HUD scale. The engine's frame — the side
  panel's width and the bars' height at the game's HUD scale, from `tagpu_hud_live` — gives the
  anchors, and the 9-slice and grid-sliced parts stretch to fill what is left, except an `xtile`
  part's x band, which repeats (*How it is built*).

## How it is built

| File | What it holds | Thread |
|---|---|---|
| `tagpu_hudstate.c`/`.h` | whether the HUD draws; the packet's HUD record filled; the record turned into the layout's state, the teams, the names and the dump; the drawn frame's hit map, handed back in the command record | game (fill), render (present, the map) |
| `tagpu_hudinput.c`/`.h` | the input: the hit map's copy, the press and release, the wheel, the commands, the HUD's own state (the held key, the folds, the scroll, the toast, the scale) and Leave's pause | game |
| `tagpu_hudmarks.c`/`.h` | the timeline's markers and the stats window's samples: the walker over the recording, and the bookmarks in `replays\index.dat` | game; the walker its own thread |
| `tagpu_replay.c` | the unit types' army values for the walker (`tagpu_replay_army_values`); the pause icon's stub (`pause_icon_init`) | game |
| `tagpu_menu.c` | the modal test `tagpu_menu_modal`, and Leave's question opened from the DrawGameScreen observer behind its guard | game |
| `tagpu_shield.c`, `wndproc.c`, `tagpu_input.c` | the mouse paths into `tagpu_hudinput_button` and `tagpu_hudinput_wheel`; the device tokens `ddown:`, `dup:` and `dwheel:` | game |
| `tagpu_hudlay.c`/`.h` | `hud.js`'s placement and builders in C, over `layout.json` and `kit.json`, the stats window and its chart among them; the text baseline; a part op as quads; the controls as `HOP_HOT` ops | either: a pure function |
| `tagpu_huddraw.c`/`.h` | the ops as instances; the glyph atlas; the chart's image; the shader pair | render |
| `tagpu_hudart.c`/`.h` | the loader: the `HUDPAK` resource, the parts decoded, the faces opened | its own, then immutable |
| `tagpu_vk_hud.c`/`.h` | the pass: the images, the uploads, one pipeline, a draw per run of instances | render |
| `tagpu_json.c`/`.h` | the JSON parser the layout reads its two files with | either |
| `tagpu_packet.h`, `tagpu_packet_pub.c`, `tagpu_packet.c` | `TAGPU_PK_HUD`, its area `hud_off`/`hud_len`, its fill, and `frame_valid`'s bounds on `nseat` and `nmark`; `TAGPU_PK_HUDSTAT` in `hstat_off`/`hstat_len`, and its bounds; `TAGPU_CMD_HUD` in the command record, and its take | game → render, render → game |
| `tools/hudpak.py`, `res.rc`, the Makefile | `art/hud/` packed into `inc/tagpu_hudpak.bin` (gitignored), embedded as the `RCDATA` resource `HUDPAK` | build |

### Where it is drawn

- **Its own pass, after the world and before the engine's UI.** `tagpu_vk.c` calls
  `tagpu_vk_hud_prepare` just before `tagpu_vk_gui_prepare`, and `tagpu_vk_hud_record` just
  before `tagpu_vk_gui_record`, after the world target's composite.
- **The HUD covers the world and nothing else.** The engine's dialogs, its messages and its menu
  draw over it, and so does the cursor, which is the last quad of the UI pass's sharp layer.
- **But for its top layer**, the ops flagged `HOPF_TOP` (the live rewind's button, which sits in
  the top bar the UI paints whole, its strip and its toast): `tagpu_vk_hud_record_top` draws them
  after `tagpu_vk_gui_record`; the whole UI is drawn again inside the Config panel's rect while
  it is open (`tagpu_vk_gui_record_whole`), and the UI's sharp layer inside their rect
  (`tagpu_vk_gui_record_sharp`), so the panel stays over the strip and the cursor on top (*Live
  rewind*, *The top layer*).
- **It is alpha-blended at the device's resolution**, premultiplied (`ONE`,
  `ONE_MINUS_SRC_ALPHA`): the parts' straight alpha is premultiplied once, at load, so filtering
  and mipmaps cannot darken an edge.
- **It blends in the swapchain's encoded values**, `B8G8R8A8_UNORM`, as the browser blends, so
  a translucent part composes in the game as it does in the mockup.
- **It claims no A/B frame, and it is never in a world capture.** The census counts it among the
  passes that drew (`hud=N` on the `vk: census` line) and not among those that claimed. Both its
  layers record in the swapchain's render pass, after the world target is composited into it, so
  it is one more pass in the captured image only when the capture reads the swapchain image: a UI
  or FPS A/B with the HUD drawn stands down rather than comparing the wrong frame, while a world
  A/B, which reads the world target, does not count it (`tagpu_vk.c`). A live recorded skirmish
  always draws the rewind button and no lever turns the HUD off there, so counting it would refuse
  every world A/B in one. The UI pass still counts: under the play defaults a world A/B needs
  `tagpu_gui.off`, as before.
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
  a directory, into `inc/tagpu_hudpak.bin` (27.0 MB with the 66 parts of 2026-10-23), which
  `res.rc` embeds. Every offset is checked against the resource's size before it is read.
- **The JSON is read at run time, not converted.** `tagpu_json.c` reads the same text the mockup
  reads, and `tagpu_hudlay_load` resolves it into structs once. A field missing or of the wrong
  type stops the load, and the log names it (`hud: NOT loaded - …`). The load also runs every
  rule once on a state that takes every branch, so a layout that loads cannot fail to build.
- **Decoding is off the frame.** The first frame that has a HUD to draw starts the loader
  (`tagpu_hudart_start`), a thread of its own that decodes the parts, premultiplies them and
  opens the faces, then publishes the result with one interlocked store; the HUD draws nothing
  until then. MEASURED 2026-10-15, before the picker's re-cut (the kit is 21.2 Mpx since): `hud: loaded 66 parts (21.5 Mpx) and 3 faces in ~830 ms`.
- **The decoded parts stay for the session**, 85 MB of RGBA. Every teardown and resize brings
  the pass down, and its images are uploaded again from these. Nothing writes or frees the
  loaded HUD afterwards, which is why any thread may read it without a lock.
- **Each part is its own image, with mipmaps.** The parts are painted at 4.2 to 12.5 pixels a unit,
  mostly about 10, and drawn at 1.1 (1080p, 75 %) to 4.5 (4K, 150 %), so most are minified 2 to 9
  times. One descriptor set per part is bound per run of quads.

### The uploads

- **The parts go once per build, across frames.** Their level 0 totals 85 MB against a slot's
  2 MiB staging cap (`TAGPU_VK_STAGE_CAP`, `tagpu_vk_stage.h`), so they take several frames:
  each frame resumes where the last stopped (`s_nsent`), a banded part uses none of a frame's
  room, and the rest go as many as fit it.
- **A part that does not fit what is left of this frame's buffer waits** to be the next frame's
  first copy. That copy goes in the frame's own command buffer when it fits the whole buffer,
  and banded when it does not. The banded ones are the nine parts over 2 MiB: `win_one`,
  `picker_one`, `rail_forks`, `rail_one`, `tp_one`, `live_one`, `foot_one`, `head_one` and `row_one`,
  sized from their PNGs. A pass's first upload after it is
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
- **The chart's image** (*The stats window*) is one RGBA32F image, 578 × 17 texels, 157 KB. It
  goes whole, in the frame's own command buffer through the slot's staging, behind the same
  barrier as the atlas's rows, whenever its generation moves (`send_chart`); a frame with no room
  for it hides the chart's instances and the next frame sends it. The first frame after the
  pass is built clears the image (`clear_chart`, `vkCmdClearColorImage` between two barriers): a
  sampled layout whose header draws no line, so every draw that binds the image binds a written
  one and the HUD never waits on the first upload's room; a slot whose empty buffer cannot hold
  the image logs `hud: the chart's image (… bytes) does not fit the slot's staging (… mapped)`
  once, and only the lines wait. A
  device that cannot sample `R32G32B32A32_SFLOAT` gets no pass (`hud: this device cannot filter
  or blit RGBA8 / R8, or sample RGBA32F - the pass stays down`).
- **A failed upload refuses the pass** (`ST_REFUSED`): a submit or a fence the device failed,
  `-1` from the stage. The pass stops drawing at once and destroys nothing mid-frame,
  where other slots' submits still name its objects. `tagpu_vk_hud_down` gives them back behind
  the seam's `vkDeviceWaitIdle` on the next `vk_down` or `passes_down`.
- **A HUD that cannot draw leaves a replay without on-screen controls for the session.** Its art
  failing to load, its pass failing to build, the device failing an upload, or its layout failing
  to build: each calls `tagpu_hudstate_refuse` on the render thread, an interlocked store
  `tagpu_hudstate_on` reads on the game thread. From then on no record is filled and no click is
  the HUD's (`hud: the HUD cannot draw (…) - a replay has no on-screen controls and a live game
  no rewind button this session`); the engine's menu still leaves. Not run: none of the four can be brought about
  without breaking the build.

### The rules in C

- **`tagpu_hudlay.c` is `hud.js`'s placement and builders, in C.** It takes the screen, the
  engine's frame and the HUD's state, and returns a list of ops: a part, a solid, a dot, a text
  or a clickable, each with its rectangle in HUD units and its clip. It reads no engine state and
  no global, so the game thread and the render thread can call it on the same inputs and get the
  same rectangles.
- **The render thread calls it to draw**, and its `HOP_HOT` ops become the hit map
  (*Input*). The game thread does not lay out again: it tests the drawn frame's map, so a click
  is tested against what was on screen.
- **Grid-sliced parts are drawn cell by cell** (`tagpu_hudlay_expand`). A part expands into at
  most `HUD_MAXQUAD` = (2·`HUD_MAXBAND` + 1)² quads (`tagpu_hudlay.h`), the bound the loader
  enforces on each part's bands. The mockup composes a grid part into one image instead; at a
  cell's edge the two can differ by a pixel's filtering, and nowhere else.
- **An `xtile` part's x band repeats** ([The replay HUD's layout](hud-layout.md), *Parts*): each
  copy is one more quad with the band's own texture coordinates, the count rounded from the drawn
  band's length over the painted one exactly as `hud.js` rounds it. The copies are fitted to what
  `HUD_MAXQUAD` leaves once every cell has its quad, so a band drawn longer than that holds fewer,
  longer copies and no cell is dropped.
- **Verified on 2026-10-15** (MEASURED, the reference setup: a private 3840 × 2160 Xvfb, a
  four-seat 2v2 and a ten-seat five-team recording on Town & Country and Core Prime): the game's
  capture against the mockup fed the game's own dump at shift 0, 0, as the share of pixels
  differing by more than 48 levels — at HUD 150 the timeline 1.50 % and the transport 2.24 % (the
  world above them and the clock's text edges), the stats window 0.35 %; the column differs only
  where the world shows between its rows and where the game shows a hover the dump does not
  carry. At 125 and 75 the timeline's copies fall at the same pixels in both; the scroll bar of
  the ten-seat column matches, thumb included.
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
  replay's speed and pause, the HUD's scale, up to ten seats (each with `pov`, its camera
  recorded) and up to 320 markers, and the HUD's own state as its input keeps it
  (`tagpu_hudinput_fill`): the pointer in client pixels (`px`, `py`; -1 when not known), the
  folded teams, the four switches (`toggles`), the column's scroll as asked, the recorded seats
  Sight and Follow are set to (-1: the full map, the free camera), the key held down and the
  toast. The render thread turns the pointer into HUD units for the hover, and Sight's and
  Follow's seats into rows.
  `frame_valid` checks the length and bounds `nseat` and `nmark` by the record's arrays before
  the render thread loops over them. A record that does not fit the slot sets
  `TAGPU_PK_TRUNC_HUD` (`0x800000`), and that frame draws no HUD.
- **Each seat comes from the replay's image of its player record**, `W->player[seat]`, through
  `tagpu_replay_players` (`tagpu_replay.c`): the name, metal and energy stored, income and
  spending, the two storage maxima, the units alive, the commander-loss counter and the
  alliance row (the offsets are the [engine map](exe-reverse-engineering.md)'s player-record table). The colour is a
  palette index, `TAGPU_REPLAY_PLAYER.colour`: the middle pixel of the seat's logo frame in the
  engine's colour sequence `*(main+0x148DB)`, the logo bounded by the sequence's count and the
  frame decoded only when it is at most 64 × 64 (`logo_colour`, `tagpu_replay.c`), taken once a
  playing for every seat (`seat_colours`, which the overlays share). The render thread resolves
  it through the presented palette.
- **"Lost"** is a commander-loss counter above 0 (the skull); **"out"** is no unit alive (the
  dimmed row).
- **The markers** come from `tagpu_hudmarks.c` (`tagpu_hudmarks_get`): the commanders' deaths and
  the defeats as far as its walker has read the recording, then the bookmarks. The walker is a
  thread with its own reader of the recording, started by the first fill that finds the playback
  live; it marks a death where a seat's commander-loss counter `+0x106` rises and a defeat where
  its live count `+0x144` falls to 0, at most 256, and publishes each entry before the count that
  covers it. **It walks the timeline that plays** (`tagpu_replay_timeline`): it chooses it before
  its first read, as the playback does, and crosses each fork on its chain by a seek to the
  iteration after the world's (`recr_tick` answers `RECR_BOUNDARY` there). When a rewind moves
  the playback to another timeline, the next fill stops and joins it and starts it again. The
  teardown wrap's entry (`tagpu_reclaim.c`) stops and joins it before `tagpu_replay_level_end`,
  while the recording it reads is still the level's. A stop waits for the reader's open, which
  reads every block header of the file, then for one iteration or one fork's crossing at most —
  a seek that scans the headers again and rebuilds from the last snapshot before the fork
  (`SNAPSHOT_TICKS`, 1,800, apart as written) plus the child's first iteration. Bounded by the
  file and the format, not a timeout. **Open (LOW): that bound is up to about 1,800 iterations of
  decode**, and `walk_stop` waits for it with `INFINITE` on the game thread
  (`tagpu_hudmarks.c`, the LIFETIME comment and `walk_stop`): at the teardown's entry or on a
  timeline change, a stop that lands in a crossing's rebuild holds the game thread for one seek's
  read, 261–265 ms on a light four-player hour and 538–550 ms on a heavy one at worst, about 2.5 s in
  a dense battle [INFERRED from the seek measurements in
  [making recordings smaller](tadr-port/demo-recorder-size.md#l4-the-measured-hours-the-snapshot-interval-and-r1)].
  The worker looks at its stop flag only between iterations and crossings. A marker outside the recording's span, or for a
  seat the HUD does not show, is dropped.
- **The teams are the mutual alliances**, joined transitively with union-find
  (`teams`, `tagpu_hudstate.c`): two seats are one team when each one's alliance row marks the
  other; a lone player is a team of one. Whether teams get headers is the layout's rule
  ([The replay HUD's layout](hud-layout.md), *The column*): a free-for-all has no team header at
  all, and a 2v1v1 gives each lone player a header of its own. MEASURED 2026-10-09 on two
  recordings of the four-seat test bed (ally groups 5 5 5 5, and 0 0 1 2): the free-for-all's
  column is four rows with no header and the 2v1v1's is Team 1 · 2/2 over two rows, then Team 2
  and Team 3 at 1/1 each over one, both matching the mockup fed the game's dump at best shift 0.
  In a skirmish the alliances are the SKIRMISH screen's: the seat fill `0x47A760` marks allied
  every human or AI seat that shares an alliance group other than 5 (the store at `0x47A890`;
  [engine map](exe-reverse-engineering.md), *The seat fill*). The group is saved in the registry as
  `Skirmish\Player<N>AllyGroup` ([command-line options](cmdline-options.md)) [INFERRED: the
  registry value reaching the settings entry's `+8` is not traced].
- **The rates** — a row's income and spending — read one decimal below 100 in magnitude and whole
  numbers from 100, rounded half up as JavaScript's `Math.round` rounds (`rate` and `js_round`,
  `tagpu_hudlay.c`; `rate`, `hud.js`). A resource bar whose storage is 0 is empty.
- **The HUD writes no engine state of its own.** Its commands go through the replay's calls
  (`inc/tagpu_replay.h`), which write the pause and the speed as
  [S5](tadr-port/demo-recorder-s5-ui.md) states, and Leave through `tagpu_menu.c` (*Leave*). Its
  one byte patch, the pause icon's stub, writes nothing (*The stats window*).

## The stats window

The stats window charts one measure over the whole recording, for each player or for each team.
Its samples come from the recording itself, read by the marker walker as it walks the timeline;
the recorder writes nothing for it.

### The samples

- **The walker samples the recording's images** (`tagpu_hudmarks.c`, THE STATS), on the same
  thread and the same iterations as the markers, never engine memory. For each recorded seat:
  metal income `+0x9C` and energy income `+0x90` (floats, the row's own), MetalWasted `+0xD4`
  (a double), units alive `+0x144`, kills `+0xFC` and losses `+0xFE` (words), and the army value.
  The offsets and their evidence: [engine map](exe-reverse-engineering.md), *The replay HUD's
  stats window and the pause icon*. `replay-layout.tsv` excludes none of them.
- **The army value** of a player is the sum, over its units whose slot is live and whose `+0x110`
  bit 28 is set, that are finished (`+0x104` 0.0, by the engine's test) with an owner below 10
  and a type (`+0xA6`) below the table's count, of the type's value: its metal cost (`def+0x18A`)
  when it moves (`def+0x22F` = 1, the test `0x4862CF` gives a unit its mover by) and is not a
  commander (`def+0x245` bit `0x40000`), else 0. `tagpu_replay_army_values` copies the values out
  of the level's defs on the game thread, `malloc`'d before the walker's `CreateThread` and freed
  by `walk_stop` after the join, so the walker never reads engine memory and never outlives its
  table. MEASURED: `279 unit types valued` on Two Continents.
- **The period** is 30 · max(1, ⌈span / (30 · 511)⌉) GameTime, span being the recording's last
  GameTime less its first: whole seconds, at most `TAGPU_PK_HUD_POINTS` = 512 samples. The base
  is the walk's first iteration's GameTime, and sample k is taken at the first walked iteration
  whose GameTime is at least base + k · period: a held value, not an interpolated one. MEASURED:
  the battle (1148 s) 383 samples 90 GameTime (3 s) apart; `hud-2v2-selfd` (102 s) 103 samples
  30 apart.
- **They are published as the markers are**: the base before the first count, each sample whole
  before the count that covers it (`InterlockedExchange`), none rewritten. The walker's log line
  ends `; N stats samples P GameTime apart, M unit types valued`.

### The packet and the groups

- **`TAGPU_PK_HUD` carries the window's state** from the input: `stat_open`, `stat_team` (the
  split asks for teams), `stat_measure` (the measure's key, 12 bytes), and `stat_x`, `stat_y`
  (its top left in HUD units, as asked).
- **While the window is open, one `TAGPU_PK_HUDSTAT`** follows in `hstat_off`/`hstat_len`
  (`tagpu_hudstate_fill_stats`, from `fill_frame`): `period`, `base`, `npt` and `v[10][512]`, the
  chosen measure's samples for the record's seats in `seat` order, as many as the walker has
  published (`tagpu_hudmarks_stats`). `frame_valid` refuses it without a HUD record, at any length
  but its size, outside the slot, or with `npt` above 512; one that does not fit sets
  `TAGPU_PK_TRUNC_HUD`.
- **The render thread groups them** (`stats` in `tagpu_hudstate.c`), as the mockup's host does:
  a player each, in its seat colour, with a cross at its first commander-death marker; or, when
  the split asks for teams and the teams are told apart (`tagpu_hudlay_headed`, the test the
  teams chip is enabled by: two teams or more, one of them with two players), a team each,
  `Team N`, summed, in `layout.json`'s `chart.teams` colours. Sample s is at t0 + s · dt seconds, dt = period / 30 and t0 = (base −
  first) / 30. A sample that is not a finite number is drawn as 0.
- **The state dump carries `stats`** in the mockup's shape — `open`, `x`, `y`, `measure`,
  `split`, `dt`, `t0` and `groups` (`name`, `col`, `death`, `v`) — so the mockup fed the dump
  draws the game's own chart.

### The draw

- **`tagpu_hudlay.c` builds the window and its chart** as `hud.js`'s `windowHtml` and `chart`
  do ([The replay HUD's layout](hud-layout.md), *The stats window*, *The chart*). Four new kinds
  of op: `HOP_LINE`, an antialiased stroke with butt ends; `HOP_CHART`, the chart's lines;
  `HOPF_DROP` on a solid, a CSS outer box shadow; and `HOPF_BASE` on a text, an SVG `<text>` on
  its baseline at its anchor.
- **The chart's lines are data**, one RGBA32F image (`tagpu_huddraw_chart`): row 0 the globals
  (the counts, the line's and the halo's half widths, the halo's colours, the cross's size), row
  1 + g group g (its colour, its halo and cross flags, its points and, per column of eight
  segments, the y range its lines can reach). The instances are bands of those columns, so a
  pixel no line reaches is never shaded.
- **The fragment stage composites the groups per pixel**, in order — the halo of a dark colour,
  then the line with round joins and butt ends, then the cross — and the whole at the op's
  opacity, as SVG's `<g opacity>` does: where lines cross in the faint part after the playhead,
  the top one shows, not the two summed.

### The pause icon

DrawGameScreen draws the GAF `igpaused` in the middle of the world while the pause bit is set,
over the window. `pause_icon_init` (`tagpu_replay.c`, THE PAUSE ICON) puts a stub on its test at
`0x46A107`: the nine bytes of the `test`/`je`, checked with the `mov eax,[main]` before them,
are replaced by the same test, and when the bit is set the stub calls `tagpu_hudinput_on_screen`
and draws the icon only when it answers 0. That is a control on the screen that carries the
pause, not merely the HUD on: controls in the command record last taken, which the render thread
posts empty for a frame that drew no HUD and the in-play draw takes before anything it draws, and
the HUD's mode (`tagpu_hudstate_mode`) the replay's, or the live HUD's while a rewind owns the
level. **A live recorded skirmish the player pauses keeps the icon**: the live HUD idle is one
button with no pause on it, and answering for the whole live mode would take the icon from every
recorded skirmish. While a rewind holds the game the strip says so, and the icon in the middle of
the world would sit under it, so it goes, as it goes under the replay HUD. While the art
loads, the pass is down or the chart's first image is not yet on the device, the icon shows. Game thread, inside the draw; it writes nothing. If the
bytes differ the log says `replay: the pause icon's test 0x46A107 holds other bytes: a replay
shows it`.

## Input

The HUD takes a click where the player saw its controls. Everything on the game thread is
`tagpu_hudinput.c`'s, which names no engine memory: the one engine fact its gate needs, a menu
over the world, is `tagpu_menu_modal`'s.

### The hit map: the frame on screen, handed back

- **The render thread makes it from the frame it draws.** Once `tagpu_vk_hud_prepare` has built
  a frame it will draw, it hands the ops to `tagpu_hudstate_drawn`, which turns every `HOP_HOT`
  op into a rectangle in the window's client pixels, **topmost first** (the build paints in
  order, so the last control painted is on top), at most `TAGPU_CMD_HUDHITS` = 64 (the rest are
  logged once, `hud: N controls past the hit map's 64 are not clickable`). Counted from the
  builders, a frame has at most 53: the dock's two plates, seven transport buttons and the track;
  the column; ten rows and at most nine team headers; one hovered row's three buttons; the scroll
  bar's track and two arrows; the foot plate's five buttons; and with the stats window open, its
  plate, its title, its close button, seven measure chips and two split chips. With them go the
  timeline's playhead run (`track_x0`, `track_x1`: the track's ends inset by `track.ends`), the
  column's first item as drawn and the greatest it can be (`first`, `last`), and each row's
  recorded seat (`seat[]`). **Each drawn frame's map is posted once**: `tagpu_hudstate_cmd`
  empties it as it copies it, so a frame that draws no HUD — the pass ran and drew none, or
  `tagpu_vk_frame` returned before the pass ran (no swapchain size yet, a rebuild, a display-mode
  change) — posts `n` = 0, and a click there is the engine's.
- **The next command record carries it** (`TAGPU_CMD_HUD` in `TAGPU_CMD`, `tagpu_packet.h`):
  `tagpu_zoom_frame_end` fills it through `tagpu_hudstate_cmd` beside the zoom's fields. It is a
  level, as the record's other levels are.
- **The game thread copies it at the take** (`tagpu_hudinput_take`, from the command record's
  apply in `tagpu_packet_pub.c`), before every in-play draw, bounding `n` and `nseat` by the
  arrays. A click between two takes reads this copy. A take with no record the exchange vouches
  for (`tagpu_cmd_take` NULL: the exchange stopped, or no record yet) empties it.
- **It describes the frame on screen.** The overlay driver, which posts the record, runs before
  `tagpu_vk_frame` in `render_vk.c`, so the map a record carries is the previous frame's — the
  one being shown. That is by design: the map is one frame behind the HUD being built, and level
  with the picture.
- **Keys are the layout's own.** A control's key is its `act` in `layout.json`, and a row's or a
  team's carries its index in the drawn frame: `jump:2` is row 2's Jump, `fold:0` the first
  team's fold. `seat[]` turns a row into the recorded seat the replay's calls take.

| Key | Where | What a click does |
|---|---|---|
| `rew`, `fwd` | transport | a seek 300 GameTime (10 s) back or forward, through `tagpu_replay_seek_request` |
| `play` | transport | the pause toggled; at the recording's end a seek to its start and the pause cleared (the button shows play there, as the mockup's does) |
| `speed` | transport | the next preset above the speed in force, 5, 10, 20, 40, 80 (½× to 8×), and from 8× back to ½× |
| `bookmark` | transport | the bookmark nearest now within 90 GameTime (3 s) removed, else one added at now (`tagpu_hudmarks_bookmark`, through `tagpu_replays_mark_add`/`_remove` into `replays\index.dat`, on the recording's line by name and root key, so they survive every playing's end); the toast "Bookmark at m:ss" or "Bookmark removed", nothing when the recording was not opened from `replays\` or the list is full |
| `uiscale` | transport | the HUD's scale to the next of 75, 100, 125, 150, wrapping, and into the settings store (`hud: the replay HUD's scale is N%`) |
| `leave` | transport | Leave's question (*Leave*) |
| `track` | timeline | a seek where it was pressed, one a frame while it is dragged, and one where it is released (*The press*) |
| `stats` | foot plate | the stats window opened or closed; drawn pressed while open |
| `close` | the window's close button | the window closed |
| `chip:<k>` | the window's trays | the measure `k` of `layout.json`'s `window.measures` (`metal`, `energy`, `excess`, `units`, `army`, `kills`, `losses`) |
| `chip:player`, `chip:team` | the economy tray | the split, by player or by team; `chip:team` is disabled, with no hot, when the teams are not told apart |
| `drag` | the window's title well | a press drags the window (*The press*); the release does nothing more |
| `cursors`, `clicks`, `selections`, `chat` | foot plate | the replay's switch toggled (`tagpu_replay_overlay_set`); each is on when a playing starts and never saved; drawn pressed while on |
| `fold:t` | team header | team t folded or unfolded |
| `sight:i` | a row's hover | Sight on row i's seat (`tagpu_replay_sight_request`), or, pressed already, the full map (-1, record 10) |
| `follow:i` | a row's hover | Follow row i's seat (`tagpu_replay_follow_request(seat, 0)`), or, pressed already, the free camera |
| `jump:i` | a row's hover | the camera put on row i's seat's once (`tagpu_replay_follow_request(seat, 1)`) |
| `scroll_up`, `scroll_down` | the column's scroll bar | the column's first item one up or down |
| `dock`, `column`, `row:i`, `scroll_track`, `window` | the plates | taken, and nothing done: a click on a plate between controls is the HUD's, not the world's |

Jump and Follow on a seat whose camera was not recorded (`pov` 0) do nothing but show the toast
"Camera was not recorded". A toast shows for `toast.ms` (1800 ms, `layout.json`), measured on the
game thread's tick count. Each row is a control of its own, `row:i`, painted under its hover's
buttons; it is how `tacli` finds a row to hover before clicking one of its buttons.

### The press

- **Two paths, one function.** `tagpu_hudinput_button(cx, cy, down, right)` takes a client point:
    - **The real path**, at the top of the shield's wndproc hook (`tagpu_shield_wndproc`), before
      the shield's own gate, so a player's mouse reaches it whether the shield is armed or not.
      The point is `lParam`, the client point; a double click (`WM_LBUTTONDBLCLK`,
      `WM_RBUTTONDBLCLK`) counts as a press, since the window's class may turn a second press into
      one. The Impure Config sprocket (`tagpu_menu_click`) is asked first.
    - **The injected path**, in `deliver_mouse`: only a **device-space** injection carries a
      client point (`dclick:`, `drclick:`, `dmove:`, `ddown:`, `dup:`, `dwheel:`); a click in the
      engine's own space (`click:`, `pclick:`) has no client point (`cx` < 0), never reaches the
      HUD and passes under it to the engine.
- **A press is taken only when** the map is not empty, `tagpu_hudstate_on`, `tagpu_replay_live`
  and not `tagpu_menu_modal`, and the point is on a control and not on the Impure Config panel
  or its sprocket (`tagpu_menu_owns_point`, at the engine's point the same message carries: the
  sprocket's `g_ddraw.cursor` on the real path, the injection's converted point on the injected
  one — the point `tagpu_menu_click` is asked with — so the HUD and the panel split the screen on
  one conversion, to the pixel). The panel hangs from the top right below the bar, over the
  column, and is drawn above the HUD; its clicks are answered by the engine's pump, which runs
  after this test. A press taken is dropped whole: no message and no
  virtual key state reach the engine.
- **A release is taken exactly when its press was**, whatever is under it then, so the engine
  never sees half a click; a press whose release was lost ends at the next press of the same
  button. A left click acts at the release, when the release is over the control the press
  landed on (`held`, drawn pressed meanwhile). A right press on a control is taken and does
  nothing.
- **The timeline drag:** a press on `track` seeks to the pointer at once; each packet fill then
  seeks to the pointer's x (`mouse_last_client`) when it has moved to a new GameTime, one request
  a frame, the newest replacing one not yet run; the release seeks to where it is. x maps along
  the playhead's run from the recording's first GameTime to its last, clamped to the run. A drag
  ends if the HUD stops taking clicks (`active` false at a fill).
- **The window drag:** a press on `drag` keeps the press's client point and where the window is
  drawn then: its place (the last drag's, else the layout's `window.x`, `window.y`) held on the
  last map's screen as the layout holds it (`stat_origin`), so a place kept from a smaller HUD
  scale or a larger screen, drawn clamped, moves with the first pixel of a drag. The mockup's
  host starts its drag the same way. Each packet fill, from the
  last client point (`mouse_last_client`), and the release, at its own, put the window at x =
  clamp(ox + (cx − dx) / u, 0, W − `window.w`), and y likewise with `window.h`, where u, W and H
  are the frame on screen's, from the command record (`TAGPU_CMD_HUD`'s `u`, `W`, `H`). A
  pointer outside the picture has no client point, so the window stays at the last in-picture
  one. A drag ends as the timeline's does, and on focus loss.
- **The window's state is kept for the session** (statics in `tagpu_hudinput.c`): the measure
  (`units` until one is chosen), the split (by player) and the place (the layout's until the
  first drag), never in `impure.cfg`. `tagpu_hudinput_level_end` closes it, so each replay starts
  with it closed.
- **Focus loss ends a drag** where it is: `WM_KILLFOCUS`, `WM_CAPTURECHANGED` and
  `WM_ACTIVATEAPP` 0 call `tagpu_hudinput_cancel`. The release, should it come, is still consumed.
- **The wheel**, `tagpu_hudinput_wheel`, runs before the zoom's in both `to_game` (the shield's
  injected path) and `fake_WndProc`, at the pointer `mouse_last_client` holds. Over the column or
  its scroll bar each notch scrolls one item, down for a notch toward the player; over any other
  control it is taken and does nothing; over the world it is not taken, and the zoom has it.
  Under the same gate as a press, the panel's rectangle included, tested at the wheel's game
  point, the one the zoom is given.
- **The scroll** is asked from the scroll last asked, not the one last drawn, so notches that
  arrive within one frame add up, and is held to [0, `last`]; the layout holds it there again
  when it builds.

### Leave

- **The click pauses and queues the question.** It remembers whether the playback was paused,
  pauses it, and sets a request; when the menu's hooks did not arm (`tagpu_menu_leave_ready`) it
  shows the toast "Leave from the game menu" instead.
- **The DrawGameScreen observer opens it** (`menu_tick` in `tagpu_menu.c`, `leave_open`), where
  the engine's own screens are opened — the engine's Exit → Main Menu path, EXITMENU's handler
  at `0x46082B..0x46083A` without its `GUI_Pop`, there being no EXITMENU to pop: the question
  `*(int*)0x512FF8 = 0`, YESORNO's opener `0x460680`, then the title set with `0x4A07D0(gi,
  "TITLE", "Leave this replay?")`. It is opened outside the pump's dispatch, where no click waits
  on an answer, so nothing is answered with `ActDone` ([engine map](exe-reverse-engineering.md),
  *The in-game quit*).
- **The guard.** Before every open the observer compares `0x460680..0x4607F1` (the opener, to its
  `ret`) and `0x4605C0..0x46065C` (its `OnCommand`, through the quit tail's `ret 4`) by FNV-1a
  against the retail bytes, `0x8B2B971E` and `0x9EF8B480` (DISASSEMBLED and recomputed from
  `pristine/TotalA.exe.pristine`). On a mismatch it logs `menu: Leave refused - …` and opens
  nothing. If the opener pushed no YESORNO, it logs that too.
- **The question is open while a screen named `YESORNO.GUI` is on the GUI stack**: from the top,
  `main+0x531`, down the `+0` links, at most 32 steps, each screen's name being its record 0's —
  `[gm+4]+2`, 16 bytes, the field the engine's own compare `0x4AB060` reads (`named_on_stack`).
  Once it is gone — No pops it, and so does a click outside it — the observer puts the pause back
  as it was before the click (`tagpu_hudinput_leave_shown`). The observer runs every in-game frame
  whatever the HUD's state, so the pause comes back even when the HUD has stopped drawing. If the
  guard or the opener refused, the pause is put back at once and the toast says "Leave from the
  game menu".
- **It is not modal to the engine.** It is opened without ARMOPT under it, so bit 0 of
  `main+0x37EBE` is clear: a click outside it closes it, as one closes ARMOPT, which is a No.
  Tab opens nothing while it is up. MEASURED 2026-10-09 (*Status*).
- **Yes** runs the engine's quit tail (`0x460630`): the teardown, and the shell, which opens the
  Replays browser again after a Watch.

### The menu over the world

`tagpu_menu_modal` is the gate's engine fact: **`(*(u16*)(main+0x37EBE) & 0x865) ||
(main[0x2BEE] & 0xE0)`** — the engine's own test for deferring menu work, as
`tagpu_patches.c`'s `order_check` also makes it ([engine map](exe-reverse-engineering.md),
*Where the cancel cannot be called*) — or `YESORNO.GUI` on the stack. Bit 0 covers the options
stack (ARMOPT, EXITMENU, YESORNO, the preferences), bit 2 the chat, bit 6 SHARE. Leave's question
needs the name test: its opener `0x460680` sets neither (bit 0's setters are `0x4961C1`,
`0x49477E` and `0x45D002`), and Impure opens it without ARMOPT under it. A build menu
stacks on the base screen without either test, so a name compare of the top screen would not do
[INFERRED: the build pages' bits not traced].
Under a menu every click is the engine's: MEASURED 2026-10-09 (*Status*). The Impure Config
panel sets neither bit, so it is not part of this test: the press tests its rectangle instead
(*The press*), and the HUD goes on answering clicks outside it while it is open.

### Driving it: `tacli hud`

`tacli hud <i> show|wait|click <key>|hover <key>|wheel <key> <n>|scrub <from> <to>|drag <key> <dx> <dy>`
(`tools/tacli`, `cmd_hud`) drives the HUD as a player's hand does — a device-space injection at a
control's place in the frame on screen — and never calls the replay's API:

- **It reads the state dump** (*Checking it against the mockup*): it removes the old answer,
  writes `tagpu_replayhud.dump`, and reads `tagpu_replayhud.json` once the game has taken the
  trigger. The dump's `hits` object is the hit map of the frame on screen,
  `{"track": [x0, x1], "first": …, "last": …, "at": [{"key": …, "r": [x0, y0, x1, y1]}, …]}`,
  topmost first, and the state carries `held`, `sight`, `follow`, `scroll`, `toast` and `stats`.
- **`click`** writes `dclick:` (`drclick:` with `--right`) at the control's centre, **`hover`**
  `dmove:`, and **`wheel`** `dwheel:x,y,n`, n notches (positive: away from the player, and the
  column scrolls up). A row's button (`jump:2`) shows only under the pointer, so tacli hovers its
  row (`row:2`) first and waits for the button to appear in the dump.
- **`scrub`** presses on the track at one fraction of the recording (`ddown:`), moves in
  `--steps` steps `--step-ms` apart (`dmove:`), and releases at the other (`dup:`).
- **`drag`** presses at a control's centre (`ddown:`), moves by (dx, dy) client pixels in
  `--steps` steps `--step-ms` apart, and releases there: `tacli hud <i> drag drag 200 -80` drags
  the stats window by its title. The window follows the pointer once a frame, so on a slow
  display a drag whose later steps leave the picture ends where the last frame sampled an
  in-picture step: keep every step inside the picture.
- **`show` and `wait`** print the state and every control, and a `stats:` line while the window is
  open (its measure, split, place, groups and samples); in a live recorded skirmish, `live,
  rewind <state>, t … of … s, arm <n>` and the controls instead. They wait up to `--timeout`
  (20 s) for an answer, which only the Vulkan lane's HUD gives.

The replay harness's W lane drives the HUD through it ([completeness](tadr-port/demo-recorder-completeness.md),
*The replay test harness*): the speed, play, the trigger's `hud seek <T>` (the timeline's own
seek entry, at a GameTime of the test's choosing), Sight twice on row 0, Follow, Follow again and
Jump on the row of the seat the recording's human commanded, Cursors off and on, play again,
and Leave, answered Yes through `tacli ui`.

## The recorder calls

What the HUD asks the recorder is declared in `inc/tagpu_replay.h`, *THE HUD'S RECORDER CALLS*,
all on the game thread: the takeover's plan and request, the timelines of the playing's file with
the axis they span, a fork's deletion and export, the live rewind's verbs and state, and the last
request's outcome, whose serial lets a caller see each outcome once; the timelines carry a serial
of their own, which moves when their rows or axis change. All are called between two
sim steps, never inside one. A call that changes anything queues it for the frame visit; the plan
commits nothing but reads the file's block headers three times (four where the segment holding
T holds anything), so it answers a click and is never
called per frame. The trigger's `takeover`, `rewind`, `hud seek` and `hud fork` run through the
same calls, and `hud state` prints what they answer (`tagpu_replay.c`, the trigger's comment). The
details: [the takeover](tadr-port/demo-recorder-s4-takeover.md#driving-it),
[the forks](tadr-port/demo-recorder-s4-forks.md#tickets-and-the-huds-route) and
[live rewind](tadr-port/demo-recorder-s4-rewind.md#the-verbs-as-a-queue-and-their-outcomes). The
live HUD calls the rewind's verbs and state, the outcome and, for Play from here, the takeover's
plan (*Live rewind*); no control of the replay HUD calls them yet.

## Live rewind

The mockup's live mode (`tools/hudart/mockup/?mode=live&rw`, hud.js `live`), drawn by the game
in a live recorded skirmish: **a rewind button beside the Impure Config sprocket** in the top bar,
which opens the strip at now and closes it back to now, and **the strip** under the bar while a
rewind is open or in the past — rew, play, fwd, the track from the recording's first GameTime to
now, "now", the time, **Play from here** and **Back to now**. The verbs are the rewind's own
([live rewind](tadr-port/demo-recorder-s4-rewind.md#the-huds-route)), sent through
`tagpu_replay_rewind`, so a click and the `rewind` trigger run one code path at one point of the
frame visit.

### The mode, the record and the layout

- **`tagpu_hudstate_mode`** (*When it draws*). The live record (`fill_live`) carries the clock,
  the pause, the scale, the rewind's state, its span (`first`, `last`) and `rw_G`, where it opened;
  no seats, no markers, no stats. It never calls `tagpu_hudmarks_get`: in the past
  `tagpu_replay_live` is true, and the markers' walker would start on the live recording.
- **The render half** (`present_live`): the strip is drawn while the state is OPEN or PAST, the
  button is down while it is not IDLE, `T` is `(rw_G − first) / 30` and `t` the playhead held to
  `[first, rw_G]`, both in seconds from `first`; `playing` is 0. The drawn track's span,
  `first`..`rw_G`, goes into the hit map (`t_first`, `t_last`), and a click on the track is mapped
  on it, then held to the rewind's span `[first + 1, last]` on the game thread.
- **The layout** (`live`, `tagpu_hudlay.c`, a port of hud.js `live`): the button at
  `(W − (entry.r + entry.s)·k, entry.t·k)`, `entry.s·k` square, with `k = top / entry.bar` — the
  bar's own pixels, as the engine places the sprocket (`trigger_rect`, `tagpu_menu.c`: x
  `w − 44`, y 2, 28 square), so the button sits 3 bar pixels left of it. The sprocket is the
  engine's: the mockup's backdrop carries it, and the HUD draws none. **The strip's play is drawn
  disabled** (`off` in `layout.json`, the mockup too): the rewind holds the past paused and has no
  verb that plays it toward now (the owner's choice for this round). Play from here is drawn
  pressed while it is armed. The toast draws in both modes.
- **The hit map's bound** is the larger of the replay HUD's and the live HUD's (the button, the
  strip's plate, its transport, its track and its actions: 7); the load's self-test builds the
  live HUD closed and open with Play from here armed, each against the live bound.

### The input

`act` hands every key to `live_act` while the last fill was the live HUD's. The table:

| key | when | does |
|---|---|---|
| `rewind` | idle / OPEN / PAST | OPEN / CLOSE / NOW; nothing while the writer flushes or a fork ends the original, where the verb would be refused |
| `rew` | OPEN, PAST | SEEK 300 GameTime back, held to `first + 1` |
| `fwd` | PAST | SEEK 300 GameTime forward; NOW when that reaches `last` (G − 1 or before), since a seek there stops short of the world the rewind opened at. Drawn disabled while the playhead is at the strip's end, which is the rewind OPEN and not yet moved (`off_at_end` in `layout.json`, the mockup too) |
| `track` | OPEN, PAST | press and drag: a SEEK a frame at the pointer, the latest winning (the queue) |
| `live_play` | OPEN | CLOSE, toast "Playing on from m:ss" |
| `live_play` | PAST | **Play from here**, below |
| `live_now` | OPEN / PAST | CLOSE / NOW |
| `live` | | the strip's plate, taken |

Any key but `live_play` and the plate ends Play from here's arm. Each outcome of a verb the HUD
sent is read once by its serial (`tagpu_replay_result`): a refusal toasts "Rewind refused" and
ends the arm, the fork "Playing from m:ss". A verb refused at once — no level in play, as after
the level's end, which clears the queue — toasts the same and ends the arm, since no outcome
follows. The takeover's plan scans the file, so it is asked only on Play from here's first click,
never per frame.

**Play from here** (owner decision 1-B: what is taken over is what is shown). The rewind's fork
starts at `takeover_point(T)`, the refresh at or before the last seek's target, which in a
player's recording (a refresh every 300 GameTime, 10 s) can be up to 10 s before the world on
screen. So the first click asks `tagpu_replay_takeover_plan(now + 1)` — in the past the playhead
is held at the seek's target − 1, so `now + 1` is the T the fork would use:

- refused: "Cannot play from m:ss";
- R is that T and nothing would be deleted: FORK, one click;
- else the arm takes R and the fork it would delete and SEEKs to R when R is earlier (the snap
  back); the arm is *seeking* until the world on screen is R − 1, then *armed*, drawn pressed;
  the second click sends FORK. The arm keeps the timelines' serial the plan left
  (`tagpu_replay_timelines_serial`, read after the plan, which refreshes the cache): when it moves
  before the second click the file's forks are no longer the ones the plan counted, so the arm ends
  and the next click plans again, the rule the takeover's picker follows.
- **The warning is the arm's**, and like every live toast it is drawn in the top layer and, while
  the strip is open, clear of it: at `top + live_clear` units, or the layout's `toast.top` when
  that is lower (hud.js `toast` places it the same way; laycheck's live toast cases). **Every toast
  is as wide as its text**, `toast.pad` each side, never narrower than `toast.w` nor wider than
  `toast.max_w` (150 and 260 units), its text cut at the pad when longer: "Click again to play from 0:30, replacing F2" sets 174 units
  of Oswald 700 at 10, past the 150 the box used to be. The layout measures the text through the
  measure tagpu_hudart.c gives it once the faces are open (`tagpu_hudlay_measure`,
  `tagpu_hudtext_width`), the same advances and kerning tagpu_huddraw.c draws the text with
  (`tagpu_hudtext_pens`); hud.js measures the same face in a canvas, and laycheck's native build
  measures with the game's own code (its eviction case at 4K agrees within 0.005 units).
  "Click again to play from m:ss[, replacing F<n>]" starts when the
  arm is set, at R, and is held for as long as the arm holds, past the toast's 1.8 s. The snap's
  seek can outlast the toast (2341 ms measured 2026-10-23 on the battle test group), and the
  warning names the fork the second click deletes, so it is on screen whenever that click can
  come. Every other toast of the live HUD ends the arm first, so while armed the toast is the
  arm's.

**The ordering it rests on** (one thread): FORK is sent only armed, which needs the snap's seek to
have run, so the rewind's seek target is R; the fork verb's own `takeover_point(R)` on the same
reader answers R, so the fork starts at the world shown, by the function the plan ran, and the
eviction shown is the one the fork acts on. The plan reserves nothing.

### The top layer

The engine's UI composite is one full-screen quad that paints every texel the engine drew, the top
bar included, so a HUD op inside the bar drawn before it would be painted over. The button's ops
are flagged `HOPF_TOP` and drawn after the composite:

- **Two builds, one atlas.** `tagpu_vk_hud_prepare` calls `tagpu_huddraw_begin`, which empties
  the glyph atlas if a glyph did not fit, then builds the base layer and the top layer into one
  instance range, the top's batches after the base's. An atlas emptied between the two builds
  would leave the base layer's glyphs naming emptied texels; emptied only before the first build,
  both layers are one generation, which the upload rule covers.
- **The record order** (`tagpu_vk.c`): the base layer, the UI composite, the top layer, the whole
  composite again inside the Config panel's part of the top layer's rect while the panel is open
  (below), then the composite's **sharp layer again** inside the top layer's rect (the union of
  the quads its instances emit, their `ndc`, held to the attachment)
  (`tagpu_vk_gui_record_sharp`): the
  composite's pipeline, set and viewport with a second quad whose vertices carry `a.z = 1`, which
  `LAY_FS` reads as `sharpOnly` and discards past its sharp-layer test. Draws in one render pass
  land in record order, so the sharp layer — the only cursor — is the last thing drawn at every
  pixel of the rect, as the composite's own is everywhere else. The composite's quad has
  `a.z = 0`, so its output is unchanged. The redraw runs only when the composite was drawn this
  frame for this slot. No new GPU object; it is all `record`, on the render thread.
- **The hit map lists the top layer's controls first**, then the rest topmost-first.
- **The strip and the live toast are the top layer too**, always, so nothing the engine draws
  into the composite can cover them. The engine's message lines are drawn in the world's top
  left (`DrawChatText 0x464060`, DISASSEMBLED: the first line's top `0x34`, each next one a font
  height down; a line of kind `0x0A` has its text at x `0x8A`, any other kind an icon box at x
  `0x8A`, `ftol(0.8 · h)` square with `h` the font height, and its text at `ftol(138 + 1.5 ·` that
  side`)`), under the strip's left end; with the strip in the base layer, the runner's 1080 capture
  at HUD 100 showed four "Under Attack" lines hiding rew, play, fwd and the track's start.
- **The message lines start under the strip** while the last frame drawn had the strip, and only
  for DrawGameScreen's call, so none is hidden under it and none shows in the gap between the side
  panel (x 128 at 1080) and the strip's left end (x 146): with the strip in the top layer alone,
  the first glyphs of each line showed there (x 138 to 145, round 2's captures). A stub on
  DrawChatText's store of its first line's top (`0x4640D6`, `chat_top_init`, `tagpu_replay.c`)
  stores `tagpu_hudinput_chat_top`'s value instead of `0x34` when its return address is
  DrawGameScreen's (`0x469FD0`, after the call at `0x469FCB`), and `0x34` for the two other
  callers: `0x41F9D8` is in `0x41F7F0`, which loads `MSGBOX.GUI` (`push 0x502BD4` at `0x41F97C`); `0x41F7A5` is in `0x41F760`, gated on `main+0x39057 == 1`, whose address appears nowhere in the exe, so how it is reached is not known [INFERRED: an address computed at run time, or not at all]. Neither is measured under the strip; the gate stores `0x34` for both. The game thread sets that value at the hit map's take
  (`tagpu_hudinput_take`), which runs in DrawGameScreen's hook before the draw that calls
  DrawChatText. **Only a map whose HUD pass ran moves it** (the map's `pass`, set by
  `tagpu_vk_hud_prepare`'s first call and cleared by each record): a render frame `vk_present`
  did not prepare posts an empty map that says nothing about the screen, which still shows the
  last frame presented, so the lines keep that frame's verdict instead of jumping to `0x34` for one
  draw. A map with `pass` gives `0x34` when that frame drew no strip, or, when it has the strip's
  plate (`live`), the first engine row clear of the strip,
  `32 + live_clear · scale · gh / 720`. `live_clear` is the layout's `2·strip.t + strip.h`, the
  strip's gap under the bar again under it; `scale` the HUD's own; `gh` the engine's height. A
  HUD unit is the attachment's height / 720 times the scale, and the bar is the frame's `bh`
  scaled by the attachment's height over `gh`, so the window's size cancels out and the row is
  exact in the frame's pixels; the world region is drawn `bh − 32` lower than the engine draws
  it, which gives the engine's row. Every way the HUD stops drawing gives `0x34`: on GDI and once
  the HUD is refused the mode is NONE on the game thread (`tagpu_hudstate_on`), which the take
  checks first, with no map needed; with no level to show no packet is filled, and the pass that
  runs builds no state and posts an empty map with `pass` set; a replay's map has no strip; the
  level's end resets it. GDI draws no HUD, so its lines never move. If the bytes at `0x4640D6` or the
  call at `0x469FCB` differ the log says `replay: DrawChatText's first line 0x4640D6 or its call
  0x469FCB holds other bytes: the messages stay under the live HUD's strip`, and the strip still
  covers them.
- **The Config panel stays over the strip.** It hangs below the bar's right end (`panel_rect`,
  `tagpu_menu.c`), over Play from here and Back to now. **Its box travels with the frame that drew
  it**: at each flip the UI mirror's flip marker takes `tagpu_menu_panel_box` (the panel's box on
  the screen while it is open, `panel_rect` moved by the world region's shift as the composite
  moves it) into that flip's `PK_FRAME`, the drain keeps the last frame's box beside its presented
  surface, and the hand-over carries both (`TAGPU_GUIHAND::panelL..panelB`). The panel opens and
  closes only at DrawGameScreen's entry (`menu_tick`), before the frame draws, and the retained
  GUI blit draws it on every frame it is open, so the box says whether the composite drawn holds
  the panel. The Vulkan pass maps the box to the attachment as the composite maps the surface
  (`tagpu_vk_gui_panel_rect`) and draws the whole composite again inside its part of the top
  layer's rect (`tagpu_vk_gui_record_whole`), after the top layer and before the cursor's redraw.
  In that rect what the engine drew is the panel, so the panel is the last UI drawn there, from
  the frame it opens to the frame it closes: the box and the composite are one snapshot. One
  frame can still disagree: the engine pops `RENDER.GUI` itself inside DrawGameScreen
  (`UpdateIngameGUI 0x491D70`, at 21 call sites, pops every screen the expected-name buffer does
  not name), and the menu learns of it only at the next frame's entry, so that frame's box says
  open over a composite with no panel. The outcome is harmless: `LAY_FS` writes only where the
  mirror has coverage and discards elsewhere, so where the panel was the strip shows, and anything
  the engine drew there instead is drawn over the strip as the panel would have been. A point the
  sprocket or the panel owns is the menu's (`tagpu_menu_owns_point`).
- **No other engine screen reaches the strip** (MEASURED on b590bfd4's runs, 1080, HUD 100): the
  in-game menu is at x 10 to 112, inside the side panel; the Exit question is centred, below
  the strip; the chat's input field is in the bottom bar; with the strip open chat does not open,
  and with chat open the strip does not open. The F4 or SPACE Kills/Losses box, which the engine
  draws at the viewport's top right (`0x469F65`, `0x469F9F`), would sit under the strip while it
  is open; not measured (*Gaps*).

### The tests

U's HUD cycle clicks the HUD on the live level before its fork (`tarewind.py`, `hud_cycle`): the button
opens the strip, with fwd not clickable there, and closes it unmoved; Play from here in the open
strip closes it; in the past rew seeks back twice, fwd seeks 10 s forward, a drag on the track
released at its right end seeks to the span's last GameTime, and from there fwd returns to now;
the button and Back to now each return to now from the past. Play from here's "Playing on from"
toast lasts the layout's 1.8 s of wall time, so no dump is sure to fall inside it, and the cycle
does not check it. U-E, on a copy of the recording, takes over timeline 1, forks it by a rewind, and
plays Play from here past a refresh of that fork: the snap back to the refresh, the eviction
named in the toast, the second click's fork there with the named timeline deleted and the fork
EQUAL to its recording (`play_from_here`, on a takeover fixture, whose refreshes are on the
30-GameTime grid). A fork's `recorder: recording timeline` line comes at its first capture, the
first step after the resume, so U-E reads it after running the held level, as U does.
The pause icon's rule is checked by eye: neither a peek nor `tacli hud show` sees the icon.
`tacli hud show` prints `live, rewind <state>, t … of … s, its scale …%, the game's HUD …%, arm
<n>` and the dump carries
`mode`, `live` (`{open}`) and `rw` (`state`, `arm`, `R`, `evict`, `G`, `first`), which the
mockup's `?state=` draws.

**The span is the chain's.** A playing of a fork seeks, clamps and starts over within its own
chain: the span runs from the root's first GameTime to the chosen timeline's last, which the scan
that chose it found (`first_iteration`). For the original nothing changes, its last being the
summary's. MEASURED 2026-10-23 on the rewind lane's `battle` recording: a HUD seek to 5401 in the
fork's playing goes to 5401, past the original's last, 5101.

## Checking it against the mockup

- **Touch `tagpu_replayhud.dump` in the game folder.** The render thread looks for it every 30
  frames (`tagpu_hudstate_present`), deletes it, and writes `tagpu_replayhud.json` through a
  `.tmp` file and `MoveFileEx`: the state in the mockup's own shape and in HUD units, with the
  device's size, the pixels a unit is (`px`) and the engine frame's scale (`q8`), the stats
  window and its groups among it. Two scales: the state's `hud` is the mockup's field, the replay
  HUD's own scale (its scale button, 0.75 to 1.5), which the live HUD keeps too; the game's HUD
  scale is `q8` / 256 (`hud: ARMED … stored 150` in the log is that one). The log says `hud: state dumped to tagpu_replayhud.json: …`.
- **The mockup draws exactly that state** given `?state=<url of the json>`
  (`tools/hudart/mockup/index.html`). It loads the shipped faces from `art/hud/fonts/` with
  `@font-face`, so a glyph the faces lack is missing there as it is in game.
- **The check:** capture the game (`DISPLAY=:94 import -window root g.png` on the private Xvfb),
  capture the mockup fed the game's dump
  (`tools/hudart/capture.sh "<mockup url>?state=<json url>" m.png`), and compare the two at native
  4K, region by region, for the best shift.
- **Where the markers come from in a test:** a recording in which a commander dies. In a
  live skirmish, `ctrl+c` selects the commander and `ctrl+d` self-destructs it. Once the walker
  has read the recording it logs what it found, `hud: markers: N commander deaths at <T> (seat
  <S>), M defeats at <T> (seat <S>), … iterations walked in … ms`; on `hud-2v2-selfd` that is one
  death at 1677 and one defeat at 2906, both seat 0. The same line ends with the stats samples
  (*The stats window*).

## The landings

1. **The HUD draws** — built. The pass, the archive, the parser, the rules in C and the text,
   drawing the dock and the column from the replay's data. Checked as *Status* says (2026-10-08).
2. **The HUD replaces the dock and the cards** — built. The layout's controls for input (the
   five-bay foot plate, the column's scroll, Sight and Follow drawn pressed, the toast, each row a
   control); the hit map, the input and the commands; Leave behind the engine's YESORNO; the
   marker walker and the bookmarks moved to `tagpu_hudmarks.c` and the seat colours to
   `tagpu_replay.c`; then `tagpu_dock.c`/`.h`, `tagpu_cards.c`/`.h`, `DOCK.GUI`, their art in
   `art/ui/`, the HUD-scale regions that magnified the two (`uDock*` and `uCards*` in the UI
   composite's shader, whose uniform block is now 80 bytes, and the `OP_FLIP` words that carried
   their scale) and the lever deleted. `tacli hud` and the W lane drive it. Checked as *Status*
   says (2026-10-09).
3. **The stats window** — built. The walker's samples of the recording's images, with no
   recorder change; the window's state and the chosen measure's samples in the packet; the window
   and its chart in C, with the measures, the team colours and the chart's text style moved into
   `layout.json` so the mockup and the DLL read one source; the line, chart, drop-shadow and
   baseline-text ops; the stats button, close, the chips and the title drag; the pause icon's
   stub; the dump's `stats`, `tacli hud drag` and the `stats:` line. Checked as *Status* says
   (2026-10-09).

Each landing is reviewed at `high`: it adds a pass and a hand-over between the game thread and
the render thread.

## Gaps

- **GDI has no stats window**, as it has no replay HUD.
- **The samples are held values, not interpolated**: each is the first walked iteration at or
  after its time, at least 1 s apart and at most 512 a recording, so a long recording's chart
  steps every few seconds (3 s for a 19-minute game).
- **The chart grows as the walker reads**: until the walk reaches the recording's end, the lines
  stop where it is.
- **The window drag stops following outside the picture**: a pointer past the letterboxed
  viewport has no client point, so the window waits at the last in-picture one until the pointer
  comes back or the button is released.
- **No capture button.** The foot plate's old capture bay is gone; a screen capture is a later
  landing's.
- **GDI has no replay controls**, by the owner's decision: the packet is the HUD's only way to
  the screen, and only the Vulkan lane reads it. A GDI player leaves through the engine's menu and
  has no on-screen seek, pause, speed, Sight or Follow.
- **The dump answers every 30 frames**, so at a low frame rate `tacli hud show` can miss a toast
  that lasts 1.8 s: at about 5 frames a second a dump is 6 s apart. Check a toast with a capture.
  Play from here's warning is the exception: it holds while the arm holds, so every dump that
  shows the arm shows it.
- **The hit map is one frame behind the HUD being built**, by design: it is the frame on screen.
  A control that moves between two frames (a row's buttons appearing under the pointer, the
  column scrolling) is clickable where the player saw it, not where the next frame draws it.
- **Bookmarks do not survive a playing** [INFERRED from the code, not run]: `replays\index.dat`
  keys them by the file's name and size, and every playing's end touches the file, which appends a
  timeline table and so changes its size; a fork, a deletion and an export change it too.
- **The replay's timeline click is mapped with the latest fill's span** (`seek_at`,
  `tagpu_hudinput.c`), not the drawn axis the hit map now carries (`t_first`, `t_last`, which the
  live strip's track maps on). That is exact while the span never changes inside a level; a
  timeline switch would change it.
- **The replay HUD no longer draws during a live rewind into the past**, where
  `tagpu_replay_live` is true: the mode asks the rewind first, and a rewind always shows the strip.
- **The live rewind's owed checks** (the rest was run, *Status*): the window sizes the HUD
  supports below 1080, the smallest above all; the F4 or SPACE Kills/Losses box under the open
  strip; and [INFERRED] that the engine draws no control in the bar under the button
  (`W − 75 s .. W − 47 s` at the game's HUD scale s), a check by eye at the smallest window.
- **Live rewind paths no test drives**: Play from here's one-click fork (the plan's R is the
  playhead's T and nothing would be deleted), which needs the past held on a refresh of the live
  recording the harness cannot name before the file is closed; the "Playing on from" and "Cannot
  play from" toasts, which last 1.8 s of wall time; a verb refused at once with no level in play;
  and the arm's end when the timelines' serial moves, which no rewind path moves while armed.
- **A multiplayer recording's fork may not play** [INFERRED from the code, not run]: the reader
  refuses a chain whose segments differ in RT_HOLD, a multiplayer root has none, and its fork,
  recorded by a live skirmish, writes one.
- **A one-pixel Oswald residual.** At 26 of 7554 sizes swept, Chrome rounds Oswald's ascent or
  descent differently from the rule the DLL follows, and the line sits a pixel off the mockup's
  ([The replay HUD's layout](hud-layout.md), *Text*).

## Open

- **The HUD font** is not decided; Oswald and Barlow Condensed are the mockup's.
- **Address space.** The decoded parts stay for the session, 85 MB of a 32-bit process, and
  the archive stays mapped with the DLL, 27.0 MB. The art loads on the first frame with a HUD
  state, which since the live rewind is the first frame of a live recorded skirmish on the Vulkan
  lane too, not only of a replay. The parts are painted at about twice the
  4.5 pixels a unit the HUD is drawn at on a 4K screen at 150 %, so most of both is detail no
  screen up to 4K shows. The owner keeps the full resolution. 20 of the 66 parts (34.4 MB decoded,
  10.7 MB of the archive) are the `proposed` section's: the Take over button and picker and the
  fork lanes, which the DLL packs and decodes but draws only once their landings make it read
  them, and the live rewind's button and strip, which it draws.
