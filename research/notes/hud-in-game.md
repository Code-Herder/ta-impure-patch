# The replay HUD in game: the plan

## Status

**Planned on 2026-10-08; nothing of it is built.** The replay HUD of
[The replay HUD's layout](hud-layout.md) is to replace the watching dock and the player cards of
[S5](tadr-port/demo-recorder-s5-ui.md) on the Vulkan renderer. It reads the same files as the
mockup, `art/hud/`, and implements the same rules, so the two stay 1:1.

The owner's decisions (2026-10-08):

- **A pass of our own, not a UI library**, with stb_truetype for the fonts. The reasons are in
  [The replay HUD's layout](hud-layout.md), *Status*.
- **The HUD keeps its own scale and fits the space the engine's frame leaves.** The replay HUD's
  scale button sets the unit; the engine's side panel and bars, at the game's HUD scale, set the
  anchors (*Units and the screen* there).

## How it is built

### Where it is drawn

A new pass, `tagpu_vk_hud.c`, recorded in the swapchain's render pass **after the world and
before the engine's UI composite** (`tagpu_vk_gui_record`, `tagpu_vk.c`).

- **The HUD covers the world and nothing else.** The engine's dialogs, its messages and its menu
  draw over it, and so does the cursor, which is the last quad of the UI pass's sharp layer.
- **It is alpha-blended at the device's resolution**, premultiplied: the parts' straight alpha is
  premultiplied once, at load, so filtering and mipmaps cannot darken an edge.
- **It blends in the swapchain's encoded values**, `B8G8R8A8_UNORM`, as the browser blends, so
  a translucent part composes in the game as it does in the mockup.
- **A capture with the HUD on holds the HUD as well as the pass it tests.** The A/B lever
  refuses a frame where a pass drew without claiming it (`tagpu_vk.c`, *the A/B capture*), so an
  A/B run in a replay stands down rather than comparing the wrong frame.

### The art and the layout in the DLL

- **One archive, embedded as a resource.** The Makefile packs `art/hud/` (`layout.json`,
  `kit.json`, `parts/*.png`, later the fonts) into one file with a directory. `res.rc` embeds it
  as `RCDATA`. The generated file is gitignored, as `inc/tagpu_art_gen.h` is. The PNGs travel as
  they are, 7.5 MB, so the DLL grows by that much.
- **The JSON is read at run time, not converted.** A small parser of our own reads the same text
  the mockup reads, and resolves it into structs once, at load. A field missing or of the wrong
  type stops the HUD from arming, and the log names the path. No converter means no second
  statement of the schema to keep in step.
- **Each part is its own image, with mipmaps.** The parts are painted at 3 to 26 pixels a unit,
  mostly about 9, and drawn at 1.1 (1080p, 75 %) to 4.5 (4K, 150 %), so most are minified 2 to 8
  times. The mipmaps are box-filtered on the CPU at load, from the premultiplied pixels. One
  descriptor set per part is bound per run of quads.
- **Decoding is off the frame.** The PNGs decode to 22.9 MB of RGBA. That happens once, before
  the pass arms, not inside a frame.

### The rules in C

- **`tagpu_hudlay.c` is `hud.js`'s placement and builders, in C.** It takes the screen, the
  engine's frame and the HUD's state, and returns a list of quads: a part, a source rectangle, a
  destination rectangle, a colour and a clip. It reads no engine state and no global, so the game
  thread and the render thread can both call it on the same inputs and get the same rectangles.
- **The render thread calls it to draw; the game thread calls it to hit-test** a click against
  the same rectangles.
- **Grid-sliced parts are drawn cell by cell**, nine or more quads sharing their edges. The
  mockup composes them into one image instead; at a cell's edge the two can differ by a pixel's
  filtering, and nowhere else.

### The text

- **stb_truetype**, vendored as one header under `inc/`. It is C, public domain (or MIT), and
  is the rasteriser inside Dear ImGui and Nuklear.
- **The faces are committed cut down** to the characters the HUD draws, with the OFL text and
  the copyright lines, and ship inside the archive.
- **Glyphs are rasterised at the exact pixel size each style needs** into one coverage atlas,
  rebuilt when the unit changes (a new resolution or replay scale). Upper case, letter spacing,
  the drop shadow and the four-copy outline are done as the mockup does them.

### The data and the input

- **The game thread publishes the HUD's state in the frame packet**, the sanctioned way for
  anything of the engine's to reach the render thread (`tagpu_packet.h`). It carries the clock,
  the replay's speed and pause, the players, the teams, the markers and which button is held.
  The render thread draws from that copy and from the mouse position, which it may read
  (`mouse_last_client`), for hover.
- **Clicks are consumed in the shield's chain**, as the dock's are (`tagpu_shield.c`): a press on
  the HUD and its release never reach the engine. Hover is not consumed; the engine sees every
  move, as it does today.
- **The commands are the dock's**: seek, pause, speed, bookmark, Leave, the sight request.
- **Two fields have no source yet:**
    - **storage capacity**, for the resource bars and the *stored / storage* text: not in
      `TAGPU_REPLAY_PLAYER`; the engine's player record has it;
    - **the teams**: not in `TAGPU_REPLAY_PLAYER` either.

## The landings

1. **The HUD draws, behind a lever.** The pass, the archive, the parser, the rules in C and the
   text, drawing the dock and the column from the replay's data, with no input. Under the lever
   the old dock and cards do not open. **Checked** by capturing the game and the mockup at the
   same screen and state, at 4K with both scales at 150 and at Auto. The mockup is given the
   game's own state, dumped by the game as JSON, so the two pictures hold the same names and
   numbers.
2. **The HUD replaces the dock and the cards.** Input, hover, the commands, the scroll; the lever
   goes, and the old dock's screen and drawing are deleted. Whether the GDI renderer keeps a
   HUD is decided here: GDI presents the engine's 8-bit surface and runs no pass of ours.
3. **The stats window.** It needs a per-second stats track the recorder does not write yet, so it
   follows a recorder landing.

Each landing is reviewed at `high`: it adds a pass and a hand-over between the game thread and
the render thread.

## Open

- **The HUD font** is not decided; Oswald and Barlow Condensed are the mockup's.
- **Startup cost**: decoding 22.9 MB of RGBA and building the mipmaps. To be measured in
  landing 1. If it is long, it moves to a worker thread at DLL load.
- **Address space**: the decoded parts are freed once uploaded. The archive itself stays mapped
  with the DLL, 7.5 MB of a 32-bit process.
