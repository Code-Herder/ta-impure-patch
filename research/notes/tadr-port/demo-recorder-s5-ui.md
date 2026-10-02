# Demo recorder S5 — the browser and the controls (the plan)

## Status

**Planned 2026-10-01. Landings 0 to 2 are built on the S5 branch and not landed**: the art
pipeline, the shell (REPLAYS and the browser) and the watching dock (*The watching dock*). This is
the plan for the
[skirmish slice](demo-recorder-skirmish-slice.md)'s S5: the Replays browser, the timeline bar,
perspectives, camera follow, the overlays and the recording switch in the settings screen. It
covers how the screens are drawn, where their art comes from, what each mod gets, what S5 needs
from the recorder, and how the work is cut into landings. The owner settled the design in a
throwaway prototype and then in an interview on 2026-10-01; the decisions are in
*Decided 2026-10-01*. The prototype (`tools/replay/prototype-replay-ui.html`, commits `7dcdd3ba`
and `74f56dfd` on the S5 branch) is deleted before the first S5 landing.

**Evidence tags.**

- **MEASURED**: measured during the prototype, with the conditions given.
- **DISASSEMBLED** / **BINARY-VERIFIED**: cited from the note that established it.
- **INFERRED**: not proved.

## What the owner picked

Three screens, each chosen from three prototyped directions:

- **The main menu (direction A, without the collar):** a REPLAYS button among the existing buttons,
  in the slot below the centre, on a plate like its neighbours'.
- **The Replays browser (direction A):** the Load Game screen's layout — the recording list on the
  left, the selected recording's minimap, players and result on the right, and Play, Delete and
  Back below. A row shows the map, the players, the length and its badges (favourite, incomplete,
  forks); a recording with forks expands into its timelines.
- **Watching (direction D):** a dock along the bottom of the world with the transport (rewind,
  play/pause, forward, speeds), the timeline strip with its markers (deaths, defeats, bookmarks,
  forks) and the playhead, and the Overlays, Timeline and Leave buttons; player cards on the right
  edge with Jump, Sight and Follow per player.

The **in-game settings panel** is renamed **Impure Config** (it was *Render options*) and gains two
rows, *Record games* and *Keep replays*.

## Decided 2026-10-01

| Question | Decision | Why |
|---|---|---|
| How the dock, cards and browser are built | **Engine GUI screens plus blits**, as the Impure Config panel is: `.gui` screens written into `impure-patch.ufo`, the engine doing buttons, pressed states, hit-testing and quickkeys (it has no hover state: MEASURED 2026-10-01, landing 2), the DLL blitting only what a `.gui` cannot express (markers, the playhead, a card's live numbers) | The panel already proves the pattern non-modal and live over the world ([renderers](../renderers.md) §2.10); its traps — the re-push on every world click, the zoom transform — are known and fixed |
| The timeline strip | **One rect the DLL owns**: the dock's screen draws nothing there; the DLL draws the strip and hit-tests press, drag, release and marker clicks, and `tagpu_zoom` passes its points through untouched the way it does for `tagpu_menu_owns_point` | An `id=4` slider would paint the SLIDERS kit, add its own one-pixel arrow buttons and cannot make markers clickable ([gui-gadgets](../gui-gadgets.md) §2.4.1, §2.6) |
| The recording list | **A stock `id=2` listbox** for selection, scrolling, the scroll bar, the wheel and the keyboard; the DLL blits the badges and the right-aligned length over each visible row from the list's own `top` and row height; the favourite star is hit-tested in its column | Everything a list does is the engine's; only the decoration is ours |
| Recordings' summaries and the player's marks | **`replays\index.dat`**, one rebuildable file: a summary per recording keyed by file name and size, the favourite flag and the last-played time; a recording that does not match is re-read from its own header and end block; written atomically (a temporary file, then a rename) | Opening the screen reads one small file, and every summary can be rebuilt; only the marks live in the index alone |
| *Record games* and *Keep replays* against the panel's rule that every row is live | **Both rows are live.** Off stops the recording now (the file ends, its end block saying why); On starts one at the current tick from a snapshot. Lowering *Keep replays* is in force at once and shown on the Replays screen; files are deleted when the next recording starts, oldest non-favourite first | Keeps the rule without an exception; a cycling click never deletes anything |
| The key that opens the live game's timeline | **None.** The rewind button beside the sprocket, or the victory/defeat screen | `R` is TA's Repair order, and a free key is free only until a mod claims it |
| Per-mod behaviour | **A per-mod table**, matched on the mod's **name** and guarded by its **content** (*The per-mod table*) | Exact for the mods we know; a DLL's size changes with every build of a mod, so it cannot identify one across versions |
| Where the art comes from | **Our own art, painted per style; nothing cut from a mod's art**, neither at runtime nor in git (*The art*) | Cutting letters and plates out of each mod's files rested on thresholds and positions tuned on six setups, and a mod that broke one would get a garbled screen with nothing to notice it |
| Text | **Never painted into art.** Labels are engine labels; the title is set from a string with the style's title font when the DLL writes `impure-patch.ufo` (*Text*) | Text in code can change without a new Codex round |
| The minimap of a recording | **Stored in the recording** (*What S5 asks of the recorder*) | The browser needs nothing from the player's install, and a map the player no longer has still shows |
| Mod information in a recording | **Name, version and a fingerprint** (*What S5 asks of the recorder*) | The browser can say "ProTA 4.8" and still tell two builds with one version string apart |
| A recording from another mod | **The browser lists the running mod's recordings**, with a toggle that shows the others, locked | Playback rebuilds unit records by type ID, so another unit set would show the wrong game |
| The recorder's side of S5 | **Asks to the recorder's owner**, not S5 edits | One writer of `inc/tagpu_recfmt.h` |
| Landings | **Shell, then dock, then streams** (*Landings*) | Each is usable on its own |
| Verification | **Retail every landing; the six setups in the release suite** (*Verification*) | — |

## The art

### Styles

A **style** is a set of our own art drawn in the look of a family of mods. There are three:

| Style | Used by | Why it is one style |
|---|---|---|
| **Retail** | retail, Total Mayhem, ProTA, TA Zero, and any mod the table does not name | their `bitmaps/dloadgame2.pcx` and `bitmaps/singlebg.pcx` are retail's, byte for byte (MEASURED, MD5 over the six compatibility setups) |
| **Escalation** | TA Escalation | its own Load Game and Single Player art |
| **Twilight** | TA Twilight | its own Load Game and Single Player art, red lettering |

Every mod overrides `anims/commongui.gaf`, so the engine-drawn widgets (buttons, the list's scroll
bar, checkboxes, sliders) already look like each mod's own; a style only has to agree with them.

### What a style carries

| Piece | Size | Notes |
|---|---|---|
| The Replays screen's ground | 640×480 | painted with recesses where the engine draws the list, the buttons and the minimap |
| The title font | A–Z and 0–9 | a GAF font shaped like `hattfont12.gaf`: one frame per character code, the baseline in the frame's hotspot |
| The main menu's REPLAYS plate | about 115×35 | the empty button recess, pasted into the slot below the centre |
| The dock kit | tiles of about 64×32 | a tileable strip, end caps and a card frame, laid out by code at any width |
| The glyphs | 28 sprites | dock icons, timeline markers, list badges and the rewind button's four states |

**The glyphs as they stand** (one set, drawn 2026-10-01): 5,645 pixels in all — 6.6 KB as an
uncompressed GAF, **4.5 KB** with GAF's run-length rows, about 0.9 KB deflated (MEASURED). They are
redrawn per style.

**The size budget.** A painted 640×480 ground as busy as TA's own measured about 126 KB deflated (a
re-laid main-menu backdrop, MEASURED); the target for ours is **under about 60 KB deflated per
ground**, which a design without dithering reaches. Three styles then add a few hundred KB to a DLL
of about 2.1 MB.

### Layouts: a style's rects travel with its ground

Each ground paints its boxes where its artist put them, so the Replays screen has one layout per
ground, and each layout is a screen of its own in `impure-patch.ufo`: retail's `REPLAYS.GUI` and
Escalation's `REPLAYSE.GUI` (`tagpu_replays.c`, `L_RETAIL` and `L_ESCALATION`). The table holds the
title bar, the list and its scroll bar, the minimap, the map's name and the players' two columns,
the timelines, the count and footer bars and the four buttons; the GUI writer, the detail and the
badges all read the one open. A style opens the screen of the ground it opens: Twilight's and an
unknown mod's are retail's, and a style whose ground falls back to retail's takes retail's rects
with it, never one without the other. The count and footer labels are centred in their bars when
the screen opens, from the height of the font then in force (the label draw puts a line's top at
the record's y, `0x4A5A7B`).

Escalation's ground measures (640×480, edges inclusive..exclusive): title bar 21..348 × 11..41,
count bar 444..618 × 20..39, list well 27..340 × 58..353, minimap box 370..448 × 59..143, map and
players box 464..614 × 59..144, timelines box 370..614 × 163..353, footer bar 21..618 × 372..397,
and button plates at y 431..456, x 21..113, 123..215, 225..318, 328..410 (unused) and 529..618.

**A button is drawn at one of the engine's face sizes, not at its record's.** A plain button with
no sequence of its own name takes commongui's `BUTTONS0` and the frame, of 0, 4, 8, …, nearest its
record's width and height ([gui-gadgets](../gui-gadgets.md) §2.3). The sizes are 16×16, 321×20,
120×20, 96×20, 112×20, 80×20 and 96×31, in retail's `commongui.gaf` and in Escalation's alike
(MEASURED). Escalation's plates are 89 to 93 px wide and 25 high: 96×20 is the nearest face but
overruns a 92-px plate by 2 px a side, so each holds an 80×20 face, centred.

### Drawing it

- **Codex paints it**, given the mod's own art as a **style reference only** — its letter shapes,
  bevels, colour ramp and heights. Sending a mod's art to Codex was approved by the owner for this.
- **Without dithering**, so Classic++'s restorer has nothing to undo and both looks agree.
- **Exact palette colours and binary alpha.** Only the palette entries that every known palette
  shares: Total Mayhem's `palettes/palette.pal` differs from retail's in 26 entries (11–15,
  144–159, 240–244) and ProTA's in 10 (11–15, 240–244) (MEASURED); the others match retail's.
- **A copy check** compares every drawn piece against its reference and refuses a run of pixels
  identical to it, so what is committed is drawn, not traced.
- **Reviewed by the owner** in an artifact before it is committed.

### Where it lives and how it reaches the DLL

The PNGs are committed under `art/ui/<style>/` as ordinary git blobs, with one `.publish-allow`
class for the folder — not Git LFS, which would leave only a pointer file in the repository and so
hide the pixels from the pre-commit hook and `publish-check.py`.

`make` bakes them with `tools/artbake.py`, written against the standard library alone (`zlib` and
a small PNG reader), since the build's Python is the bare `python3` CI installs: PNG → palette
indices → GAF → a generated header that is not committed, compiled into the DLL. The bake refuses a
colour outside the shared entries and any partial alpha. At start-up the DLL writes the running
style's files into `impure-patch.ufo` beside `render.gaf` and the two Visuals screens'.

## Text

Captions, column headings, button labels and the dock's labels are ordinary `id=5` labels and
button texts: the engine sets them in the mod's own `hattfont11`/`hattfont12`.

The engine's largest face is 17 px (`armfont`, `hatt14`, `gaf12`; `tools/guifont.py --list`),
against about 30 px for a painted TA title, so the **title** uses the style's own font. The DLL
sets the string into the ground when it writes `impure-patch.ufo`. A label's font is chosen through
the screen's `id 7` resources ([gui-gadgets](../gui-gadgets.md) §3); whether one can name a GAF font
rather than a `fonts/*.fnt` face is not known, and if it can, the title becomes an ordinary label.

## The per-mod table

**Identification.** An entry is matched on the mod's own name — its config file or main archive
(`prota.ini`/`prota.gp3`, `mayhem.ini`/`mayhem.gp3`, `taesc.ini` with the `guiE` folder,
`tazero.ini` with `ZGui` and `FrontendZ`) — never on a file's size or hash, which change with every
build. Twilight has no file of its own name (*Open*).

**The guard.** An entry's moves apply only when the thing they move is where the entry expects it:
a checksum of the source pixels, recorded in the table beside the rectangles. A mismatch — a new
version that redrew its art, or a mod the table does not name — takes the stock placement, logged,
and never a half-applied layout.

**The entries** (positions in the 640×480 shell; MEASURED against live captures of each setup's
main menu):

| Mod | What changes when REPLAYS is added |
|---|---|
| retail, Escalation | REPLAYS at (272,393), 96×20, on our plate at (263,386) |
| ProTA 4.8 | its badge, painted into its backdrop at (283,392)–(356,438), moves up to (283,338), just above REPLAYS, and our plate takes the slot; its version label moves up 54 px with it |
| Total Mayhem 11.3.0 | its version label, drawn at run time centred on x 320 at y 418–425, moves to y 371, just above the plate |
| TA Zero | none: its `ZGui/mainmenu.gui` comments the Credits gadget out with `/* */` and the engine honours the comment, so the slot is empty |
| TA Twilight | its buttons have no plates: REPLAYS at x 317 with no plate, INTRO and EXIT 24 px to the right |

How the main menu gains its gadget and the backdrop its plate is landing 1's first research
question: the mod's own `MAINMENU.GUI` and `FrontendX.PCX` are what the engine loads, so either the
DLL serves a generated copy of the screen through `gui_redirect`, or it adds the gadget and pastes
the plate after the engine has loaded them.

## The screens

**Over the world, the dock and the cards** are GUI screens pushed the way the Impure Config panel
is (`tagpu_menu.c`): non-modal, re-pushed when a world click rebuilds the in-game stack, and owning
their points against the zoom transform. A gadget draws only inside its own panel's surface, so the
dock is its own panel along the bottom and the cards a panel on the right edge. The things only we
draw go through the engine's GAF blitter `0x4B7F90` after the GUI, as the sprocket does.

**The Replays browser** is a shell screen of its own: our ground, the engine's list and buttons over
it, the minimap and the badges blitted.

**Impure Config** gains *Record games* (On/Off) and *Keep replays* (a count), on the panel's 28 px
row pitch, with the caption **Impure Config**.

## The watching dock

Built as landing 2 (`tagpu_dock.c`, with `tagpu_menu.c`'s slot, `tagpu_replay.c`'s requests and
`tagpu_replays.c`'s bookmarks); the engine facts are in the [engine map](../exe-reverse-engineering.md),
*The watching dock*.

**What it is.** On a level that plays a recording, Watch's or `tacli replay play`'s, `DOCK.GUI`
sits right of the side panel and above the bottom bar: screen x from the panel's width + 8 to the
screen's width - 8, its bottom 4 px above the bar, 86 px tall (136, 598, 1136, 86 at 1280×720). It
does not open narrower than 400 px. Under HUD scale its screen rect is moved back by the world's
shift, as the Impure Config panel's is, and it is drawn at 1x: MEASURED at 1920×1080, HUD scale
Auto (225 %) and zoom 2.13, record 0 at (136, 878, 1616, 86) and a strip click at 25 % seeking to
25 % of the recording.

| Row | Gadget | What it does |
|---|---|---|
| 1 | `TNOW`, the strip, `TEND` | the time now (the drag's target while dragging) and the length, `m:ss` or `h:mm:ss` of GameTime / 30 from the recording's first; the strip, which is the DLL's |
| 2 | `DKREW`, `DKPLAY`, `DKFWD` | a seek 300 GameTime (10 s) back or forward; pause and play, the bit the Pause key flips |
| 2 | `DKSPEED` | 1/2x, 1x, 2x, 4x, 8x: GameSpeed 5, 10, 20, 40, 80, written to the ceiling and the live speed |
| 2 | `DKMARK`, `DKLEAVE` | a bookmark at now; Leave, through the engine's own YESORNO retitled "Leave this replay?" |

**One screen slot.** `tagpu_menu.c` holds RENDER.GUI and the dock in one slot, the one owner of
`main+0x37EA0`: RENDER wins while the player wants it, the dock comes back when it closes. The
slot lets its screen go at the teardown wrap's entry, before the engine's own pops.

**The strip.** The DLL draws it at the post-GUI observer `0x46A308` -- the fill to the playhead,
the death and defeat markers, the bookmarks, a ghost playhead while dragging, the playhead -- and
skips it while a screen above the dock overlaps it. It takes a press only while the dock is the top
screen, as the pump does, and its release is consumed exactly when its press was: a drag seeks
once, at the release, and TNOW shows the target meanwhile. A point within 4 px of a marker takes
the marker's time: 5 s before a death or a defeat, a bookmark's own. A right click on a bookmark
removes it. Losing focus or capture ends a drag with no seek.

**The markers** come from a worker thread with its own reader of the recording, which walks it
once a level: a commander death is a player's `+0x106` rising, a defeat its `+0x144` reaching 0.
They work on every recording, old and cut short ones included; ask 2 is still the browser's.
MEASURED: 2940 iterations of a 2-minute skirmish in 43-46 ms, 3990 of the 85 MB battle in 2033 ms
(0.51 ms an iteration); a Leave 1 s after the dock opened stopped the walk at 1784 iterations, and
the teardown, a second Watch and the process went on.

**Bookmarks** are one field of `replays\index.dat`, after the seats: comma-separated GameTimes,
at most 64, none within 30 of another, written through the index's temporary file and rename.

**The speed and the pause are the player's.** Neither is recorded. The dock writes both only on a
playback level; the player's speed is exchanged around every settings save and put back at the
teardown, ceiling and live speed both, so the registry's `gamespeed` and the next game keep it --
and a change the Options screen makes during a playback is put back too. A speed the dock has no
stage for is put on the nearest preset (a tie goes to the slower).

**Measured on retail** (1280×720, private Xvfb, Vulkan; the pictures also under `tagpu_gdi.on`):

- the speeds, GameTime a second over 5 s: 1/2x 15.1, 1x 30.5, 2x 59.1, 4x 118.9, **8x 150.5**
  -- 8x is as fast as the frame steps it, five ticks at most;
- seeks from the dock 6-34 ms on the small recording; a `tacli replay seek` of the battle to 2701
  in 446 ms, its dump EQUAL to the recording (`tarec compare --puppet`), and `tareplay run
  forest-fire` passing with the dock up on its playings;
- Leave and No keep the dock; Leave and Yes reach the main menu, the browser reopens, the shell's
  stack is MAINMENU and REPLAYS, and the speed is the player's again;
- the in-game menu over the dock: its buttons and the strip leave the click to the engine;
- RENDER's sprocket replaces the dock and gives it back; twenty such round trips cost 840 KB
  once, then nothing;
- no hover state on a plain button; the engine's stage captions (`1x` .. `8x`) drawn at the
  plate's left; `text=|` gives a stage button no caption.

**Gaps.**

- **The art is a stand-in.** `dock-tex`, `strip-track` and `strip-fill` are generated plain
  stand-ins; the owner's art replaces them before the landing reaches `main`.
- **Vulkan smooths the ground.** On Vulkan the stand-in `dock-tex` is drawn as an almost flat dark
  plate with a light frame; under `tagpu_gdi.on` it shows its pattern. [INFERRED: the GUI twin's
  undithering takes a fine texture for dither.] Whether the owner's texture survives it is to be
  seen with that art.
- **A greyed stage button does not look greyed.** The engine draws its stage's frame
  (`0x4A6092..0x4A60B3`), so DKPLAY and DKSPEED keep their look while the playback is not live
  (its first step, a refused playing); their clicks are refused all the same.
- **The dock is drawn at 1x under HUD scale.**
- **YESORNO does not pause**: the playback runs on behind Leave's question.

## What S5 asks of the recorder

The recording format is the recorder's ([skirmish slice](demo-recorder-skirmish-slice.md#the-file));
S5 asks for these instead of editing it:

1. **The minimap in the header.** The TNT's minimap picture, the GAF frame at `main+0x1426B`, alive
   for the whole level and NULL when the file has none (VERIFIED 2026-09-12, `tagpu_packet_pub.c`):
   at most about 252×252 bytes, deflated with the block.
2. **A summary in the end block**: each player's result, and each commander death and defeat with
   its GameTime, so the browser and the timeline need not walk the stream.
3. **The mod**: its name as the per-mod table knows it (empty for retail or an unknown mod), its
   version as the mod states it (best effort, from its own text), and a fingerprint — each archive
   the engine mounted, in the engine's order, with its name, size and CRC-32 — beside the unit-name
   hash and the weapon fingerprint the header already carries.
4. **Starting mid-game**: a recording that begins at the current tick with a snapshot, for
   *Record games* turned on during a game; and an end reason for one stopped by the player.

Items 1 to 3 are needed by landing 1, item 4 by landing 5.

## Landings

| Landing | Delivers | Needs |
|---|---|---|
| **0 — the art pipeline** | `art/ui/`, the `.publish-allow` class, `tools/artbake.py` and its checks, the make rule, the retail style | — |
| **1 — the shell** | the per-mod table, REPLAYS on the main menu, the Replays browser, `index.dat`, its dialogs | asks 1–3 |
| **2 — watching** | the dock: the timeline strip, its markers, the transport, the speeds, bookmarks, Leave (built: *The watching dock*); the timeline selector waits for S4 | — |
| **3 — the streams and the cards** | each player's camera, cursor, clicks and selection recorded; the player cards (Jump, Sight, Follow); the Overlays | a recorder change of its own, planned with it |
| **4 — live rewind** | the rewind button, the timeline in a live skirmish, Back to now, Play from here | S4 |
| **5 — the switches** | *Record games* and *Keep replays* | ask 4 |

Escalation has its own ground and plate, with retail's title font; Twilight's style is not drawn
yet and uses the retail style, which is what an unknown mod gets anyway. Each lands as it is
reviewed.

## Verification

Each landing is verified on **retail**, driven through `tacli ui` with pictures of every screen it
touches. The six compatibility setups are run by the **release suite** (`tools/compat/tacompat.py`),
which gains a Replays step: open Replays from the main menu, open a fixture recording, play, scrub
and leave, polling for error boxes, with a picture per screen.

## Open

- **How Twilight is recognised.** It ships no config file of its own name; a stable file or string
  of its own is still to be found.
- **Whether an `id 7` resource can name a GAF font** (*Text*).
- **How the main menu gains its gadget and its plate** (*The per-mod table*).
- **The dock's art.** `dock-tex`, `strip-track` and `strip-fill` are plain generated stand-ins until
  the owner's art replaces them, before the landing reaches `main`.
