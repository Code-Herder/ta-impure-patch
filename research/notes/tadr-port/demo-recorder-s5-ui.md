# Demo recorder S5 — the browser and the controls (the plan)

## Status

**Planned 2026-10-01. Landings 0 to 2 are implemented**: the art
pipeline, the shell (REPLAYS and the browser) and watching (*Watching*). Watching's controls are
the replay HUD's ([The replay HUD in game](../hud-in-game.md)), a Vulkan pass of our own that
replaced the watching dock and the player cards this plan first built; those two are deleted.
This is the plan for the
[skirmish slice](demo-recorder-skirmish-slice.md)'s S5: the Replays browser, the timeline bar,
perspectives, camera follow, the overlays and the recording switch in the settings screen. It
covers how the screens are drawn, where their art comes from, what each mod gets, what S5 needs
from the recorder, and how the work is cut into landings. The owner settled the design in a
throwaway prototype and then in an interview on 2026-10-01; the decisions are in
*Decided 2026-10-01*. The prototype (`tools/replay/prototype-replay-ui.html`, commits `7dcdd3ba`
and `74f56dfd` on the S5 branch) is deleted before the first S5 landing.

**Landing 5's two rows, *Record games* and *Keep replays*, are not built; what they switch is.**
Every skirmish and multiplayer game is recorded by default, `tagpu_recorder.on` being a play
default, and `tagpu_recorder.off` (or `tagpu_defaults.off`) in the game's folder is the only way to
stop it. Each new recording, never a fork's, prunes `replays\` to a fixed count, the newest 50
counting the new one, or the `keep N` the lever's text names; an older recording is deleted unless
it is in use, has a live fork, is a favourite or is bookmarked in `index.dat`, or does not read as a
recording ([GPU status](../gpu-status.md) §2.6e, *The prune*). That is the *Keep replays* decision
below — files deleted when the next recording starts, oldest first, never a favourite — with a
fixed count; landing 5 adds the count as a row, and recording switched on or off mid-game (ask 4).

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
  edge with Jump, Sight and Follow per player. It was built as the watching dock and the player
  cards, and redesigned as the replay HUD: layout E1, its art painted
  ([HUD art](../hud-art.md)) and drawn by a pass of its own, which replaced both.

The **in-game settings panel** is renamed **Impure Config** (it was *Render options*) and gains two
rows, *Record games* and *Keep replays*.

## Decided 2026-10-01

| Question | Decision | Why |
|---|---|---|
| How the browser is built (and how the dock and the cards were, before the replay HUD replaced them) | **Engine GUI screens plus blits**, as the Impure Config panel is: `.gui` screens written into `impure-patch.ufo`, the engine doing buttons, pressed states, hit-testing and quickkeys (it has no hover state: MEASURED 2026-10-01, landing 2), the DLL blitting only what a `.gui` cannot express (markers, the playhead, a card's live numbers) | The panel already proves the pattern non-modal and live over the world ([renderers](../renderers.md) §2.10); its traps — the re-push on every world click, the zoom transform — are known and fixed |
| The dock's timeline strip | **One rect the DLL owns**: the dock's screen draws nothing there; the DLL draws the strip and hit-tests press, drag, release and marker clicks, and `tagpu_zoom` passes its points through untouched the way it does for `tagpu_menu_owns_point` | An `id=4` slider would paint the SLIDERS kit, add its own one-pixel arrow buttons and cannot make markers clickable ([gui-gadgets](../gui-gadgets.md) §2.4.1, §2.6) |
| The recording list | **A stock `id=2` listbox** for selection, scrolling, the scroll bar, the wheel and the keyboard; the DLL blits the badges and the right-aligned length over each visible row from the list's own `top` and row height; the favourite star is hit-tested in its column | Everything a list does is the engine's; only the decoration is ours |
| Recordings' summaries and the player's marks | **`replays\index.dat`**, one rebuildable file: a summary per recording keyed by file name and **root key** (the root header block's two CRCs, `recr_root_key`, which no playing, fork, deletion or compaction changes), the favourite flag, the last-played time and the bookmarks; a line whose key is not its file's is another recording's and starts empty, and one whose file has changed size keeps its marks and has its summary re-read from the file's own header and end block; written atomically (a temporary file, then a rename) | Opening the screen reads one small file, and every summary can be rebuilt; only the marks live in the index alone |
| *Record games* and *Keep replays* against the panel's rule that every row is live | **Both rows are live.** Off stops the recording now (the file ends, its end block saying why); On starts one at the current tick from a snapshot. Lowering *Keep replays* is in force at once and shown on the Replays screen; files are deleted when the next recording starts, oldest non-favourite first | Keeps the rule without an exception; a cycling click never deletes anything |
| The key that opens the live game's timeline | **None.** The rewind button beside the sprocket, or the victory/defeat screen | `R` is TA's Repair order, and a free key is free only until a mod claims it |
| Per-mod behaviour | **A per-mod table**, matched on the mod's **name** and guarded by its **content** (*The per-mod table*) | Exact for the mods we know; a DLL's size changes with every build of a mod, so it cannot identify one across versions |
| Where the art comes from | **Our own art, painted per style; nothing cut from a mod's art**, neither at runtime nor in git (*The art*) | Cutting letters and plates out of each mod's files rested on thresholds and positions tuned on six setups, and a mod that broke one would get a garbled screen with nothing to notice it |
| Text | **Never painted into art.** Labels are engine labels; the title is set from a string with the style's title font when the DLL writes `impure-patch.ufo` (*Text*) | Text in code can change without a new Codex round |
| The minimap of a recording | **Stored in the recording** (*What S5 asks of the recorder*) | The browser needs nothing from the player's install, and a map the player no longer has still shows |
| Mod information in a recording | **Name, version and a fingerprint** (*What S5 asks of the recorder*) | The browser can say "ProTA 4.8" and still tell two builds with one version string apart |
| A recording from another mod | **The browser lists the running mod's recordings**, with a toggle that shows the others, locked. **Listing and grouping** go by the mod's name and version; **the lock** is **Watch's own refusal, remembered**: a recording Watch refused because this game's data or this build differ from it is marked, and the mark goes when the browser opens under another archive fingerprint or build (*What the browser shows of a recording*, decided 2026-10-02). The fingerprint `RT_MODPRINT` alone never locks: installing or removing a map changes it | Playback rebuilds unit records by type ID, so another unit set would show the wrong game. The shell cannot compute a level's unit and weapon hashes (the engine map, *`0x42D2E0`*): a level renames its types from their files and fills the weapon table only at its load, so the only true test is Watch's |
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
| The glyphs | sprites | list badges, the fork markers and timeline glyph for S4's selector, and the rewind button's four states |

**The glyphs as drawn on 2026-10-01**, the dock's icons and markers among them: 5,645 pixels in all — 6.6 KB as an
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
- **Without dithering, and left as drawn.** Classic++'s restorers skip Impure's own art: a GAF
  frame of ours carries `TAGPU_GAF_EXACT`, and our own PCX ground and the plate's box in the
  mod's FrontendX are marked through `tagpu_gui_asset_exact` (GUI renderer §27.5). So both looks
  agree, measured 0 px between Vulkan and GDI on the Replays ground and on the plate.
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

Captions, column headings and button labels are ordinary `id=5` labels and
button texts: the engine sets them in the mod's own `hattfont11`/`hattfont12`.

The engine's largest face is 17 px (`armfont`, `hatt14`, `gaf12`; `tools/guifont.py --list`),
against about 30 px for a painted TA title, so the **title** uses the style's own font. The DLL
sets the string into the ground when it writes `impure-patch.ufo`. A label's font is chosen through
the screen's `id 7` resources ([gui-gadgets](../gui-gadgets.md) §3), and one cannot name a GAF font
[DISASSEMBLED]: stage 1's id-7 branch (`0x4A8EF3`) builds `fonts\` + the record's text + `.FNT` and
reads that file raw into `+0xD6`, which the label draw `0x4A56B0` selects through `0x4C1420`.
Without an id-7 font a label uses the GAF `hattfont11`; only two GAF fonts are registered
(`0x4AEDD0` at `0x491564` and `0x49157D`). A generated one-colour `.FNT` cannot carry a painted
ramp, so the title stays set into the ground.

## The per-mod table

**Identification.** An entry is matched on the mod's own name — its config file or main archive
(`prota.ini`/`prota.gp3`, `mayhem.ini`/`mayhem.gp3`, `taesc.ini` with the `guiE` folder,
`tazero.ini` with `ZGui` and `FrontendZ`) — never on a file's size or hash, which change with every
build. Twilight has no file of its own name; it is recognised by its main menu's own label,
which begins "Total Annihilation: Twilight" (`TWILIGHT_PREFIX` in `tagpu_replays.c`).

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

The DLL adds the gadget and pastes the plate after the engine has loaded the mod's own
`MAINMENU.GUI` and `FrontendX.PCX`: the REPLAYS record is appended to the screen's records at
`GUI_Load 0x4AACAE` (copied from SINGLE), and the plate is pasted into the decoded backdrop on the
return of the PCX loader `0x429290` (the header of `tagpu_replays.c` lists the four sites).

## The screens

**Over the world, the replay HUD** is no engine screen: a Vulkan pass of our own draws it over the
world and under the engine's dialogs, and the DLL hit-tests its controls on the game thread
against the frame on screen ([The replay HUD in game](../hud-in-game.md)). It holds the
transport and the timeline at the bottom and the players' column on the right, and between them
the world stays the world's: a click is the HUD's only on one of its controls or plates.

**The Replays browser** is a shell screen of its own: our ground, the engine's list and buttons over
it, the minimap and the badges blitted.

### What the browser shows of a recording

Everything comes from the recording's own summary (*What S5 asks of the recorder*, items 1 to 3),
read by `recr_info` (`tagpu_replays.c`):

- **`index.dat` caches it**, so that opening the screen stays one small read. Each line gains,
  after the bookmarks, an `I` field followed by the version text, the archive fingerprint, the
  archives mounted, the mod named from them, and each seat's result, last commander death and
  defeat. A line written before these fields keeps its favourite, last-played time and bookmarks,
  and has the rest read again.
- **The minimap** is read when a row is selected, never when the index is built. The browser
  shows the recorded picture's part [0, 2·columns) × [0, 2·rows), fitted as the engine fits the
  level's minimap (`0x466780`): the long axis takes the box, the short one its share, truncated,
  and the result is centred. It is sampled nearest and drawn by the minimap gadget itself, a frame
  at its `+0xC2` ([gui-gadgets](../gui-gadgets.md), id 6). A recording without a picture keeps
  the box's grey. MEASURED 2026-10-02: Core Prime's 252×252 fills retail's 90×90 box exactly, and
  Escalation's 78×78 square at (370, 62), inside its 78×84 box.
- **The result** follows each name: "Won", "Lost", or nothing when it is not known.
- **The mod** is named from the recording's mounted archives (`RT_ARCHIVE`) by the per-mod table's
  own archive patterns. The first archive that matches an entry names it, and none means retail;
  a mod the table does not know is grouped by its version text. The running game's mod is named
  by the same rule over the live mount list, so the two always agree. The index keeps what it
  read under a generation number (`INFO_GEN`), raised whenever the table or this rule changes, so
  a line of another generation is read again; a recording the reader cannot open is asked again
  at the next open. The detail shows it with the
  version under the timelines (`MODLINE`: "Total Annihilation v3.1"). When the recording's
  fingerprint is not this install's, the next line says "The installed archives differ." That is
  a hint only. The running game's fingerprint is taken once the browser has mounted every
  archive a level mounts: the main menu of a fresh start holds fewer, since `InitTAHPIAry`
  mounts at most ten new `.hpi` per call (the engine map, *The mount list*). MEASURED 2026-10-02
  on retail: 28 archives and `CACE0F26` at the first main menu, so every recording read "The
  installed archives differ."; 31 and `4D781ECD`, the recordings' own, once the browser has
  called it until a call mounts nothing, as the main menu's MULTI calls it (`0x425E0B`).
- **The filter.** The list holds the recordings of the running game's mod and version. The
  **All mods** button, in the reserved slot (retail's plate at 340, 440; Escalation's 82-px plate
  at 328..410), shows every recording and becomes **This mod**. The count reads "3 of 4
  recordings" while some are hidden. A recording made before the mod was recorded names no
  archive: it is listed only under All mods, with blanks for the mod and the results and a grey
  minimap.
- **A recording being written** is never offered. The writer holds its file open for writing
  until the end is written, and an open that denies writers fails exactly then
  (`tagpu_replay_being_written`). Watch makes the same test.
- **The lock.** When Watch's own check refuses a recording for this game's data or this build,
  the browser marks its index line (`L`: the refusal's code, the archive fingerprint and the
  build key it was made under, and the values it compared). The marking refusals are the unit
  types (`RT_UNITDEFS`), the weapons (`RT_WEAPONS`) and the features, compared at the first step
  of the level; and another `replay-layout.tsv` or `replay-globals.tsv`, at the header check.
  While a mark holds, the row carries `b-locked` after the map name, Watch is grey on it, and
  the detail says "Made with other unit data." (or "Made with another build of Impure." for the
  last two).
  - **What removes a mark.** Whenever the browser opens, a mark made under another archive
    fingerprint or another build key goes, since the data may now match; so does a mark whose
    file has changed size. The build key is this build's exclusion file and positive list
    hashes, its wreck pool size and its weapon slots (`tagpu_replay_build_key`).
  - **Never marked:** a missing map, the recording being written, a file the reader cannot
    read, and the engine's own notices.
  - **Why only after Watch.** The shell cannot reproduce a level's unit and weapon lists (the
    engine map, *`0x42D2E0`* and the weapon load `0x42E310`). The level renames each type from
    the file its name points to, and the battleroom's restriction store narrows the set. The
    weapon table is empty at the main menu (MEASURED 2026-10-02 on retail: all 256 names).
  - **The restrictions.** A unit-types refusal marks only when the level kept every type its
    load began with: the count at the unit-data load's entry `0x42D2E0` against the level's.
    The level drops each type whose `+0x241` bit 23 is clear, and since the loader has already
    dropped every such type, one the level drops had its bit cleared by the battleroom's
    restriction store (the engine map, *`0x42D2E0`*). That is process state no key sees, so
    such a refusal is logged and not marked. The weapons, the features and the build are not
    touched by the store.
  - **What the rule cannot see.** The fingerprint is the mounted archives' names and sizes. A
    mark survives a change it does not cover: a loose unit file in the game's folder, or an
    archive replaced by another of the same size. Watch then stays grey until the fingerprint
    or the build changes.
  - MEASURED 2026-10-02 on retail, a fresh recording and three copies:
    - The copy with another exclusion file was marked at the header check. Its row turned
      locked at once, under the notice.
    - The copy with an edited `RT_UNITDEFS` passed the header check. It was refused at the
      first step ("279 here, 279 there"), marked from the empty level, and locked when Leave
      brought the browser back.
    - The copy with a missing map was refused and never marked.
    - Both marks held across a relaunch.
    - Adding a one-file `.ufo` to the game folder changed the fingerprint (`83FEE162`); the next
      open removed both marks. Removing it again left them gone, and a new refusal marked again.

**Impure Config** gains *Record games* (On/Off) and *Keep replays* (a count), on the panel's 28 px
row pitch, with the caption **Impure Config**.

## Watching

On a level that plays a recording — Watch's, or `tacli replay play`'s — the controls are the
replay HUD's, on the Vulkan renderer; its input, its commands and Leave are in
[The replay HUD in game](../hud-in-game.md), *Input*. On GDI a replay has no on-screen controls
(the owner's decision, 2026-10-09), and the engine's menu leaves. What watching does, whoever
draws it:

| Control | What it does |
|---|---|
| rewind, forward | a seek 300 GameTime (10 s) back or forward |
| play | pause and play, the bit the Pause key flips; at the recording's end, from its start |
| speed | ½×, 1×, 2×, 4×, 8×: GameSpeed 5, 10, 20, 40, 80, written to the ceiling and the live speed |
| bookmark | a bookmark at now, or the one within 3 s removed |
| Leave | the engine's own YESORNO, retitled "Leave this replay?" |
| the timeline | a seek where it is pressed, and once a frame along a drag |
| Sight, Follow, Jump | a player's view, camera follow, and the camera put on theirs once ([the replay's players and Sight](demo-recorder-player-cards.md), [the perspective plan](demo-recorder-perspective-streams.md)) |
| Cursors, Clicks, Selections, Chat | the overlays' and the chat's switches, on at every playing's start, never saved |

**The markers** come from a worker thread with its own reader of the recording, which walks it
once a level (`tagpu_hudmarks.c`): a commander death is a player's `+0x106` rising, a defeat its
`+0x144` reaching 0. They work on every recording, old and cut short ones included; ask 2 is still
the browser's. MEASURED 2026-10-01 with the same walker under the dock: 2940 iterations of a
2-minute skirmish in 43-46 ms, 3990 of the 85 MB battle in 2033 ms (0.51 ms an iteration); a
Leave 1 s after the playing opened stopped the walk at 1784 iterations, and the teardown, a second
Watch and the process went on. At most 256 markers reach the timeline; the walk logs how many it
found past that ("1 past the 2 the timeline holds", MEASURED on the battle with the cap lowered to
2 in a scratch build).

**Leave's question holds the playback.** Leave pauses it before the question opens; No puts the
pause back as it was before the click, Yes leaves. MEASURED, with the dock (2026-10-01) and with
the HUD (2026-10-09): from playing, GameTime held while asked and ran again after No; from
paused, it stayed paused after No.

**Bookmarks** are one field of `replays\index.dat`, after the seats: comma-separated GameTimes,
at most 64, none within 30 of another, written through the index's temporary file and rename. They
belong to the recording, not to a timeline, and follow its line's root key, so they survive the
timeline table every playing's end appends (the touch), a fork, a deletion and a compaction. A
bookmark is placed only inside the recording's span, and kept when a compaction after the root
timeline's deletion shortens that span; the HUD draws only those inside the span it plays.
MEASURED 2026-10-23, before the key: the index keyed a line by its size as well, and a bookmark at
GameTime 2039 was gone after Leave and the browser's reopen (the file 119847 → 119895 bytes).
MEASURED 2026-10-23 with the key (retail, a private Xvfb, a 4-seat recording of 4213 ticks
watched through the browser): a bookmark placed on the HUD at GameTime 427 was in the index after
Leave, whose touch grew the file 171974 → 172022 bytes, and on the HUD's timeline at the next
Watch; the line's favourite, carried over from a version-1 line, stayed with it.
**The index's version** is 3 (`IMPURE-REPLAYS-INDEX\t3`): after the size, the root key, then
the recorded start (`RT_WALLCLOCK` as `recr_wallclock_of` reads it: a UTC FILETIME, `none`, or `-`
not read yet), which the list is ordered by, newest first, exactly as the prune orders
(`recfile_newer_first`), before its cut to 1024. **A mark goes only when a read proves its file
changed or is gone.**

- **The index is read whole and written whole.** Every line goes into one table that grows as it
  needs (`ent_slot`), however many there are, apart from the browser's list of at most 1024; so a
  file listed again later finds its own line, and no second line is ever made for a name the index
  holds. Three kinds of line do not reach the table, or leave it: a line that does not parse is
  not read, a second line of one name is merged into the first (the favourites or'd, the
  bookmarks' union, the key unknown unless both lines have the same one), and a line whose file
  is not there goes at the next write (below). A line that does not fit the 4 MB cap fails the
  whole write, which then leaves the old index in place.
- **Names are compared exactly, then without case**, both in the C locale: a file finds the line
  of its exact name, else one of its name in another case (a file renamed in case alone), and
  never one whose exact name is another file's, which Wine allows beside it. A file whose line is
  already listed for a newer file is not listed, and logged. The prune finds a line by the same
  rule.
- **A version-1 index is converted at its first load**, text line by line, every line kept by
  the conversion, those past 1024 too, and read as the table reads its own, then written at once. Each line's key and start are written `-`, *unknown*: no
  file is read for them. The browser's next open reads both for every file in `replays\`, before
  its cut to 1024, and a file it cannot read then sorts after the dated ones, as the prune sorts
  it. The line takes its file's key at the first read of the file that succeeds, with version 1's
  own rule: the favourite and the last-played time stay, and the bookmarks and the refusal's mark
  stay while the file's size is still the line's. Until then the prune counts the line's marks by
  its name alone. Any key field that is not sixteen hex digits reads as unknown, and a file whose
  first bytes are not a whole recording header never gives its line a key.
- **A line is dropped only when its file is not there**, besides a line that does not parse and
  a second line of one name, merged: the name not found at a write, never an open that failed. A summary that could not be read now (the file held exclusively) leaves the
  line's size unknown, so the next open reads it again.
- **An index that cannot be read is never rebuilt over.** Only a missing file, or a missing
  `replays\` folder, is no index; an empty one holds no mark either. A read that fails, a file
  over 4 MB, one that is not text (a NUL in it), a version this build does not know (a later
  build's among them), or no memory to read it whole makes every write of the index refuse and
  log, until the file reads again. A favourite clicked then is put back as it was, and logged.
  The prune reads the same file itself: it keeps every recording when it cannot read it either,
  and otherwise decides from it, which no write has replaced.
- **An index written by this branch's earlier builds is version 2**, of two layouts, one without
  the recorded start; no version 2 ever shipped. This build reads it as a version it does not
  know: it is never written over, and the prune keeps every recording.
- **A downgrade is the hazard this cannot cover.** A build from before version 2 reads a version-3
  index as foreign and rebuilds it, which loses its favourites and bookmarks; nothing in this build
  can stop that.
- The prune reads a version-1 index as "nothing decided" and keeps every recording, until the
  first load converts it.

**The speed and the pause are the player's.** Neither is recorded. The replay writes both only on
a playback level (`tagpu_replay_speed_set`, `tagpu_replay_pause`); the player's speed is exchanged
around every settings save and put back at the teardown, ceiling and live speed both, so the
registry's `gamespeed` and the next game keep it -- and a change the Options screen makes during a
playback is put back too. The HUD's speed button steps to the next preset above the speed in
force, and from 8× to ½×.

**Measured on retail** (1280×720, private Xvfb, Vulkan, 2026-10-01, through the dock's buttons,
which called the same replay functions the HUD's do):

- the speeds, GameTime a second over 5 s: 1/2x 15.1, 1x 30.5, 2x 59.1, 4x 118.9, **8x 150.5**
  -- 8x is bounded by the scheduler's clamp, five ticks per step of the 30 Hz clock
  ([engine map](../exe-reverse-engineering.md#the-scheduler-the-clock-and-the-test-pacing-disassembled-measured-2026-10-08));
- seeks 6-34 ms on the small recording; a `tacli replay seek` of the battle to 2701 in 446 ms,
  its dump EQUAL to the recording (`tarec compare --puppet`);
- Leave and Yes reach the main menu, the browser reopens, the shell's stack is MAINMENU and
  REPLAYS, and the speed is the player's again.

**The harness drives watching on every landing** that touches the recorder: `tareplay.py`'s W
process on every fixture marked `"watch": true` (`stock-limits`, `perspective`, `wpn-badtgt` and
`tenseat` in `tools/replay/fixtures.json`; the
[completeness note](demo-recorder-completeness.md#the-replay-test-harness)) starts the playing
through REPLAYS and Watch, has Watch refuse two edited copies, then drives the replay HUD through
`tacli hud`: the speed, the Options' OK (the store's `gamespeed` must stay the player's), play,
the HUD's seek (the trigger `hud seek <T>`, whose dump must compare EQUAL with the recording),
Sight, Follow, Jump and a switch, play again, and Leave and Yes; `watch_refuse_engine` alone stays
unreached, since the CD check it reports never refuses in this exe.

## What S5 asks of the recorder

The recording format is the recorder's ([skirmish slice](demo-recorder-skirmish-slice.md#the-file));
S5 asks for these instead of editing it:

1. **The minimap in the header.** The TNT's minimap picture, the GAF frame at `main+0x1426B`, alive
   for the whole level and NULL when the file has none (VERIFIED 2026-09-12, `tagpu_packet_pub.c`):
   at most about 252×252 bytes, packed with the block.
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
| **2 — watching** | the timeline, its markers, the transport, the speeds, bookmarks, Leave (built: *Watching*, first as the watching dock, now the replay HUD's); the timeline selector waits for S4 | — |
| **3 — the streams and the cards** | each player's camera, cursor, clicks and selection recorded; Jump, Sight and Follow per player; the Overlays (built: first as the player cards and the dock's Overlays menu, now the replay HUD's rows and foot plate) | a recorder change of its own, planned with it |
| **4 — live rewind** | the rewind button, the timeline in a live skirmish, Back to now, Play from here | S4 |
| **5 — the switches** | *Record games* and *Keep replays* (not built; recording by default and the prune are in, *Status*) | ask 4 |

Escalation has its own ground, plate and title font; Twilight's style is not drawn
yet and uses the retail style, which is what an unknown mod gets anyway. Each lands as it is
reviewed.

## Verification

Each landing is verified on **retail**, driven through `tacli ui` with pictures of every screen it
touches. The six compatibility setups are run by the **release suite** (`tools/compat/tacompat.py`),
which gains a Replays step: open Replays from the main menu, open a fixture recording, play, scrub
and leave, polling for error boxes, with a picture per screen.

## Open

- **The remaining style.** Retail and Escalation each have their supplied replacement PNGs, 52
  each in `art/ui/`. Escalation's own title font and favourite badge were checked in its running
  `REPLAYSE.GUI` at 640×480 on the reference setup, with the mod's executable and archives.
  Twilight still needs its font, its sprite frames and its browser ground.
- **The replay HUD has one look for every mod**: `art/hud/` is not a per-mod style.
- **Live rewind** remains a proposal.
