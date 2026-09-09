# GL UI renderer — the plan for Phase E

*The engine's software frame is UI only since G13b; this page is the plan for making the UI
ours too — the side panel, the top and bottom bars, the minimap, chat and dialogs in game, and
every screen of the shell outside it — drawn by OpenGL from the engine's own draw calls, with
the same Classic / Classic++ split the world has. It records the decisions of the 2026-09-06
design interview with their reasons, the engine facts they rest on, the module boundary, the
gates with their exits and kill rules, and what a later phase must find intact. G15-0 (§8),
G15a (§9) and G15b (§10) are built and measured — G15b is the twins, drawing the in-game UI and
the shell at 1:1 under Classic — and the three land together. The gadget-tree facts it leans on are on
[GUI gadgets](gui-gadgets.html), the composite on [terrain & depth](terrain-depth.html) §7, and
the restorer on [Classic and Classic++](renderers.html) §4c.*

Evidence tags: **[SOURCE]** = read from the code named; **[VERIFIED]** = read in the
disassembly, with the address; **[MEASURED]** = a live or offline measurement with the numbers;
**[DECIDED]** = a choice made by the project owner, with the date; **[INFERRED]** = a name or
role not yet confirmed; **[OPEN]** = not settled.

---

## 1. What this is, and what it is not

**Phase 1 — the same layout at the same pixel size, drawn by us** [DECIDED 2026-09-06]. Every UI
pixel comes from our GL draw at 1:1. Under Classic the frame is pixel-identical to today's; under
Classic++ the panel art, buttons and unit pictures are restored while text stays a crisp 1-bit
glyph. The engine's 8-bit surface stops being what the player sees and becomes the *oracle* the
gates measure against and the *fallback* that shows anything we missed.

**Phase 2 — the UI scaled** [DECIDED 2026-09-06; **the interview ran 2026-09-08 — §13**, which
settles the eight questions this paragraph deferred and supersedes five decisions below]. At 1080p and above the
128-px side panel and the 32-px bars are drawn at 2× and the world viewport shrinks around them.
The engine's layout constants are fixed pixels (`left=128`, `top=32`, `bottom=H−33`, written once
at `0x4981C9..0x498237` — [resolution](resolution.html) §2), so this means running the engine at a
logical resolution and rendering the world at the device's. Phase 1 must not close that door;
§6 lists what it has to leave intact. The shell is the easy half of phase 2: its layout is
640×480 whatever the window is, so "draw the 640×480 layout into a window-sized twin with smooth
filtering" is a late gate, not a new architecture.

**Not this project**: a redesigned UI; hi-res fonts (§13.4 draws TA's own glyphs, it does not
add a font); regenerating the minimap **from our terrain atlas** — still a candidate, §13.6
regenerates it from the game's own 252-px picture instead.

---

## 2. What the engine does — the facts the design rests on

**One surface, one present.** World and UI land in one 8-bit `OFFSCREEN` (`*(main+0x37E1B)`,
`0x30`-byte header: `+0x00 w`, `+0x04 h`, `+0x08 pitch`, `+0x0C pixel base`, clip rect
`+0x1C..+0x28`) and `FlipOffscreenToPrimary 0x4C63A0` copies it to the DirectDraw primary — from
`DrawGameScreen` at `0x46A3DB` and from 43 other sites, the shell included **[VERIFIED,
[frame composition](frame-composition.html) §5]**. cnc-ddraw uploads the primary as one `GL_R8`
texture and draws it through a palette lookup; every UI pixel on screen today is that quad.

**The GUI is retained, not redrawn** **[VERIFIED 2026-09-06, objdump]**. Every `.GUI` screen owns
a pre-rendered 8-bit surface at `panel+0xBC`; the gadget handlers behind the type dispatcher
`0x4A9176` (table `0x4A962C`) render into *that*. The per-frame GUI draw is
`0x46A303: call 0x4AB170(main+0x519, &ctx, main+0x37E27)`, a thunk into
`0x4AB0B0(GUIMEMSTRUCT*, OFFSCREEN* ctx, RECT* vp)`, `ret 0xC`, which recurses down the GUI
stack (`+0x00 per_active`) bottom-up and, for each screen, blits its surface with
`0x4C6B70(ctx, panel+0xBC, panel.x, panel.y)` **only if** `Active_b (+0x14) == 1` **or** the
panel rect `(+0x13, +0x15, +0x17, +0x19)` overlaps `vp` per `0x4B67D0` [INFERRED name; the call
shape is verified]. A null `vp` takes the dirty test alone. The in-game side panel at
`[0,128,128,352]` does not overlap the viewport `{128,32,W−1,H−33}`, so it is re-blitted only
when something marks it dirty.

**The rest of the in-game HUD is per-frame into the main offscreen** **[VERIFIED,
[UI markers](ui-markers.html) §4 and the `DrawGameScreen` tail]**: the resource text block
(`DrawTextCustomFont 0x4C14F0` and four GAF blits, `~0x468E40..0x4692C0`, string
`"%dK%s %s%s E:%d M:%d"` at `0x50783C`), `DrawChatText 0x464060` at `0x469FCB`,
`DrawPopupF4Dialog 0x4948E0` at `0x469F65`, `DrawPopupButtomDialog 0x4689C0` at `0x469F9F`, the
clock and debug strings, the `LIGHTBAR` slide `0x45FFB0` at `0x46A3C2`, and the debug profiler
bars `0x46B900` ×9 at `0x46A330..0x46A3B8` (gated `main+0x38DD5` — these are **not** the side
panel or the minimap, whatever `ui-markers.md` §4 says; G15a corrects it). The panel art itself
is painted once per mode switch by `0x467D70` at `0x49842A`, the bottom bar anchored to
`ScreenH − 0x20`.

**The minimap** picture is `TED_GENERATED_PIC *(main+0x1426B)`, its rect `main+0x142E7..0x142ED`,
its view box `main+0x142CB` (filled by `0x466B70`, already ours at zoom). **Located by G15a
[VERIFIED 2026-09-07]**: `DrawMinimap 0x466B00(ctx)` at `0x46961F` in DrawGameScreen copies the
composite surface `main+0x142DB` into the frame with `0x4C6B70` and draws the view box with
`0x4BF8C0`; the composite itself (picture plus radar dots) is rebuilt from the sim side by
`0x466DC0`/`0x466C20`, not per frame. It never goes through the GUI surfaces.

**The pixel-writing leaves are known** **[VERIFIED, appendix]**: the GAF blits (`CopyGafToContext
0x4B7F90`, its shaded twin `0x4B8500`, the clipped descriptor blit `0x4B8150`, the tile copy
`0x4C6E70`), the glyph blitter `0x4CCF60` (cdecl, nine arguments, **takes its destination
pointer directly**, no context and no clip — `tagpu_text.c` already calls it), the line, bar
and hollow-rect drawers (`0x4BE950`, `0x4BF6F0`, `0x4BF8C0`, all through the store-only Bresenham
`0x4CC7AB` and the bar worker `0x4CCDEA`), and the surface copy `0x4C6B70` over
`CopyScreenContext 0x4CBBE0`. Surfaces come from `SurfaceCreateNamed 0x4C69F0(tag, w, h)` and go
back through `SurfaceFree 0x4C6AC0` **[VERIFIED 2026-09-07]**; G15a added the descriptor blit
`0x4C6D20`, the textured-triangle stamp `0x4C7580`, the framed box `0x4BF4D0`, the focus rect
`0x4BF7B0` and `SurfaceFill 0x4C6890` to the set, and found `0x4B8150` to be terrain-only
(engine map, "The UI surfaces and their writers").

**The shell is the same gadget system at 640×480**, atom-locked (`0x498025..0x498108`,
`0x491ADC..`) whatever the registry says; the requested mode is applied at game entry and undone
on exit, and each switch is a real `SetDisplayMode` that restarts cnc-ddraw's render thread with
a **new GL context** **[VERIFIED, [resolution](resolution.html) §2; MEASURED, roadmap G12]**.
Under tacli's windowed config the window is literally 640×480 in the menus. Game entry loads
`palettes/guipal` at `0x498109` for the loading screen; `palettes/guipal.pal` ships beside
`palette.pal`; whether the shell fades or swaps palettes has **never been measured** — only that
nothing cycles in play [MEASURED, terrain-depth §7; OPEN here].

**The cursor is in the back buffer only inside the flip.** `0x4C2870` at `0x46A3C7` blits it with
a NULL context — which resolves to the back buffer — and the flip itself draws it into the back
buffer with `0x4C67C0`, copies to the primary, and **restores the background** with `0x4C6B70`
before unlocking **[VERIFIED 2026-09-07, engine map "FlipOffscreenToPrimary"]**; at the flip's
entry the buffer holds no cursor, which is why a diff taken there never sees one. Since G13m it
sits under the true pointer with 0 % of motion frames left behind [MEASURED].

**The engine free-runs.** Its frame loop ran ~83 `DrawGameScreen` passes per presented frame at
1024×768 in the G13d capture (9 300–10 200 blocks per 120 presents) **[MEASURED, ui-markers
§6.2]**. Anything that records per engine frame must expect that ratio.

**The composite today** **[SOURCE `tagpu_native.c` CFS]**: inside the true viewport rect our
fragment is discarded wherever the engine's surface is not the key (index 254); outside it the
engine's frame wins unless we drew a non-empty pixel, which the world passes never do.

---

## 3. Decisions  [DECIDED 2026-09-06 unless noted]

### 3.1 Outcome: phase 1 is parity at 1:1, phase 2 is scale
§1. The one rule phase 1 carries for phase 2: **the UI twin's size is `surface × k`, `k = 1`
now**, ops are recorded in logical (game) coordinates, and nothing in the module assumes
`k == 1`.

> **Superseded 2026-09-08 (§13.2).** The twin stays 1:1 and `k` applies at the *draw*, with a
> device-res sharp layer added beside it — so phase 1's oracle diff keeps working at every `k`
> instead of becoming undefined. The rest of the rule stands: ops are logical, nothing assumes
> `k == 1`.

### 3.2 Mechanism: mirror the primitives into retained GL twins; seed from the surface as fallback
Every engine surface gets a GL twin — the main offscreen, each screen's `+0xBC` surface, the
minimap picture, whatever else the census names. Each pixel-writing leaf gets an **observer**
detour: it records what was drawn and returns to the original, so the engine keeps drawing its
own surface, which stays the oracle. Nothing is suppressed in phase 1.

A surface whose writers are all known is drawn from primitives. One that is not — or one first
seen mid-life, or every one after a GL context change — is **seeded**: its twin's content is a
copy of the engine's own bytes (§3.5). The alternatives were weighed and rejected: uploading the
engine's surfaces whole and restoring them as images (complete on day one, but it feeds text to a
model that has never seen text and makes phase 2 an upscale of a 1997 bitmap), and
re-implementing the gadgets from the live tree (full control, largest effort, and every
load-time rewrite the engine does to slider geometry or synthesised scroll arrows becomes a
divergence to chase).

### 3.3 Composite: the twin wins, the engine's surface is the fallback, `strict` removes it
Three layers, in order: **the UI twin where its coverage is set**, everywhere, viewport included
— that is how mirrored chat, `ARMOPT`, `EXITMENU` and the F4 dialog land over the world at 1:1,
untouched by zoom, which also closes the open note in [GPU status](gpu-status.html) that dialogs
over the viewport "take the transform as though they were world"; **else the engine's surface
where it is not the key** (the cursor, and anything unmirrored); **else our world**. The twin's
**viewport region is transient**: the terrain key fill is the engine's per-frame erase there, so
its mirror is "clear the twin to transparent over the true viewport rect". Outside the viewport
the twin is retained and the engine erases with primitives, which are mirrored like any other
draw.

The fallback is always on for players. A `strict` token in the trigger turns it off and paints
a miss in a sentinel colour, so a screenshot walk *counts* holes instead of hiding them. `strict`
is the harness's mode, never a player's.

### 3.4 Twin format: an index twin always, a colour twin under Classic++
Every mirrored draw writes the **palette index** into an `RG8` twin (index, coverage). Under
Classic++ the GAF sprite ops also write restored colour into an `RGBA8` twin. The composite
resolves the index twin through the **live** palette, exactly as the engine's surface is resolved
today, so `guipal` swaps and any menu fade are correct by construction under Classic. The colour
twin is used only while the live palette still equals the palette its restore snapshotted; when
it moves, that surface falls back to its index twin until the palette settles and the art is
re-restored. A fade shows dithered art for its duration, never wrong colours.

Coverage lives in the second channel rather than in a reserved index: "index 254 is unused" was
measured inside the viewport, not on GAF art.

> **Built, G15e (§14).** As built the colour twin is `COLOR_ATTACHMENT1` of the *same* FBO, so
> one MRT draw writes the index and the colour together; and the "restore snapshotted" palette
> is a real snapshot — `tagpu_rglsl_job_new` copies it into a texture of its own — so the
> validity test is a `memcmp` against the palette the frame is **presented** with, per G15d. The
> re-restore half is built too: 30 still frames and the job is rebuilt against the new palette.

### 3.5 Record on the game thread, replay all at present, seed instead of reconstruct
- **Record.** Each observer appends `(surface, op, args, engine-frame seq)` to a ring. A
  prologue detour on `FlipOffscreenToPrimary 0x4C63A0` appends a **frame marker**, the engine's
  own "this frame is complete" point.
- **Replay all, in order, at our present** — not "the last engine frame only": a panel redraw in
  engine frame 37 of 80 must not be dropped, because the panel twin is retained. *As built
  (G15b, §10)*: all in order, with two refinements the run forced — identical ops within one
  published batch collapse to their last occurrence (the shell redraws every gadget on every
  one of its ~12 000 flips a second), and a pixel op carries its box's bytes as they stand at
  publish time, so it is the final state of that box whatever wrote it.
- **Seed, do not reconstruct.** A twin created for a surface that already exists takes a copy of
  the engine's bytes, made on the game thread at the flip marker where the frame is quiet. The
  same path is the **overflow policy**: a full ring stops recording, marks every surface for
  re-seed, and the next flip re-seeds them. And it is the **re-arm path**: the module toggled
  back on, or a new GL context, re-seeds rather than replaying history.
- **GAF pixels for atlas misses are copied into the ring on first sight**; later blits of the
  same frame carry only the atlas key. The sprite atlases read frame pixels on the render thread
  from the pointer, which the shell's constant pop-and-free would turn into a use-after-free
  here. A full 640×480 background is 300 KB once.
- **Registry.** Surfaces are keyed by object address, pixel base and size, registered from the
  `0x4C69F0` allocator and the GUI surface creation, evicted on free, and lazily registered when
  a primitive names one we have not seen.

### 3.6 Three op kinds; unknown writers are caught by brackets, and counted by the census
Only GAF blits need their **identity** (the frame, for the Classic++ twin and for phase-2
filtering). Everything else only needs its **result**.

- **Sprite op**: a GAF blit with frame key, destination, colour-key and blend flags; replayed
  from the UI atlas (index) and its restored twin (colour).
- **Copy op**: surface to surface (`0x4C6B70`); replayed twin to twin. The GUI panel reaching the
  frame is one of these.
- **Pixel op**: a rect of captured indices with a coverage mask; replayed as a quad into the
  index twin and, palette-resolved, into the colour twin.

Pixel ops come from **brackets**: an observer around a unit of work with a known bounding box
snapshots the box before, lets the engine run, and records the **residual** after. Sprite ops
inside the bracket shadow-write the snapshot on the CPU (the engine's own key-skipping copy), so
the residual is exactly what nothing else explained. Two bracket kinds cover the UI: **per leaf**
(the glyph blitter, box from the font's width table; the line, bar and rect drawers, box from
their arguments) and **per gadget** (the dispatcher `0x4A9176`, box = the gadget rect,
destination = the screen's own surface), which captures every handler's output without knowing
its internals and retires `GUI_BlitToFramebuffer 0x4B0230`, the listbox renderer and the slider
knob as unknowns.

**Text and lines are captured pixels, not re-derived geometry.** G13o and G13p ported markers and
labels as geometry because the world needed them zoom-invariant. The UI is screen-space at 1:1,
so a captured pixel is exact and free, and text stays a crisp index under Classic++ because the
restorer never sees it. In phase 2 captured pixels scale by nearest — for 1-bit text the honest
1997 look at 2× — and a string op that re-renders from a nicer font is the upgrade path, added
later without touching the model.

> **Superseded 2026-09-08 (§13.3, §13.4).** Nearest staggers every 1-px feature at a fractional
> `k`, so the mirror scales by a sharp bilinear that is bit-identical at `k = 1`; and the string
> op is taken, drawing **TA's own glyphs** from `tagpu_text.c`'s atlas rather than a nicer font,
> which §1 excludes.

**The census** is the same machinery pointed at the whole surface: at each flip, diff the surface
against its previous copy and subtract every recorded op's box. What remains is a writer we have
not bracketed, with its exact pixels, per screen. It is G15a's exit and every later gate's
regression.

### 3.7 The cursor stays the engine's in phase 1
It never enters a twin (§2), so it reaches the frame through the fallback, as today. `strict`
exempts its rect (position `[obj+0x1B6/0x1BA]`, size from the sprite record at `+0x1B2`). Owning
it — drawing the sprite at present time from the true pointer and suppressing the four engine
blits — is the **first gate of phase 2**, where a 1× cursor on a 2× UI forces it.

> **Amended 2026-09-08 (§13.5).** Owning it needs no suppression: the layer's cursor branch
> changes from *discard* to *mask the fallback in that rect*, and the engine's blits may keep
> running into a frame nobody sees. The sprite record is a GAF frame header, so its size and
> hotspot are already in hand. Its size stays **1× device pixels at every `k`**.

### 3.8 The minimap is mirrored like everything else
A sprite op if it goes through a GAF blit, a pixel op through the gadget bracket otherwise; dots
and view box are residual pixels. Under Classic++ the picture is restored as one frame if it
arrives with an identity. Regenerating it from our restored terrain atlas is a candidate for
phase 2, where a 252-px picture at 2× may not be enough.

> **Decided 2026-09-08 (§13.6).** Phase 2 regenerates it — base, fog and view box ours, **the
> engine's dots kept**, since which units get a dot is fog/LOS sim logic and re-deriving it wrong
> is a multiplayer cheat. The base is the game's own `TED_GENERATED_PIC` at its native 252 px,
> which the engine halves; the terrain-atlas variant stays a candidate.

### 3.9 Classic++ on UI art: judge offline first, then a name-glob policy
The restorer was trained on ground textures and has never seen a bevel, a button or a
`unitpics` render **[SOURCE `unditherer/LEARNINGS.md`]**. So:

- **G15-0, before any engine code**: extract the shell backgrounds (`bitmaps/*.pcx`), the HUD art
  (`bitmaps/armgui{top,side,bot}tile.pcx` and CORE's), the button and small art (`ARMBUTT`,
  `CORBUTT`, `anims/loadgame.gaf`, `anims/cursors.gaf` as the expected exclusion) and a sample
  of `unitpics/*.pcx`, run `python -m unditherer restore … --preset learned --report
  --consistency` on them offline, and put original beside restored at 3× on one contact sheet
  per class, rows sorted by dither-consistency ascending, plus one whole captured menu screen
  as the cautionary column showing what text becomes if it were not excluded. The owner judges
  per class.
- **Runtime policy**: frame headers carry no name, but `0x4B8D40`/`0x4B8DA0` look sequences up by
  name, so an observer there gives a sequence-to-name registry, and `uirestore=` in
  `tagpu_classicpp.cfg` takes name globs; **the default, ruled 2026-09-08 from G15-0's sheets, is
  `all` minus `cursor*`, `pathicon` and anything under 12×12** (below the 25-px receptive field
  there is nothing to restore). The order buttons are in, despite their baked-in labels (§8).
- **Kill rule**: if bevels and panel grain smear, the exclude list grows until what remains looks
  right, and a UI-aware fine-tune (synthesised bevels and renders in the corpus) is filed as a
  candidate, not started. The Classic half is unaffected.
- The UI job takes priority 4 in the restorer's pool, which today has `MAX_JOBS 4` for
  priorities 0–3 [SOURCE `tagpu_restoreglsl.c`]; G15e raises it.

### 3.10 One module, one trigger, one seam
The UI is a subsystem, not a surface. Family `tagpu_gui_*.c`, contract `tagpu_gui.h`, trigger
`gamedir/tagpu_gui.on` with tokens (`strict` and `off` now — `off` keeps the detours installed
for the next launch while nothing is published or drawn, the live A/B; `scale=`, `nocursor` in
phase 2). §4.

- The detours install once at DLL attach if the trigger exists then, per the arming rule in
  [own the draw](own-the-draw.html), and are safe to leave resident: every one calls the original,
  so with the module off the engine's behaviour is byte-identical to today's.
- The **drawing follows the trigger live**, polled per frame like `tagpu_classicpp.on`: delete
  the file and the composite drops the twin layer that frame; recreate it and the twins re-seed
  at the next flip. Off also stops recording, so an idle module is a few `if`s.
- **Switch matrix**: `gui` off → engine UI whatever Classic++ says; `gui` on, `classicpp` off →
  our Classic UI, pixel-identical; both on → restored art under `uirestore`. The §2.10 Options
  menu of [renderers](renderers.html) later gets an "engine / GL" UI switch that creates or
  deletes the trigger — front end, not store.
- **tacli**: a `gui on|strict|census|off|remove` verb (G15b) and `gui.on` in the default arm
  set of the ta-drive skill, so the pass is armed at launch for anyone using the driver.

### 3.11 Verification: the engine's surface is the oracle
- **The strict walk.** A script under `tools/` drives one instance through a fixed screen
  inventory by gadget name via `tacli ui`, and at each screen takes the engine's surface shot
  and our GL framebuffer shot in Classic under `strict`. Exit per screen is two zeros:
  differing pixels outside the cursor rect, and sentinel-coloured holes. Inventory: the shell
  (`MAINMENU`, `SINGLE`, `SKIRMISH`, `SELMAP`, `LOADGAME`, `PREFS` and its four children,
  `EXITMENU`/`YESORNO` by key) and in game (`ARMMAIN2`, `ARMCOM1` and two more build pages,
  `ARMOPT` by Tab, the F4 dialog, typed chat, Esc's `EXITMENU`), ARM and CORE, at 640×480,
  1024×768 and 1920×1080. The multiplayer provider screens stay out: `SELPROV`'s select crashes
  the engine and has never been exercised live [gui-gadgets §9].
- **The bar is 1024×768; 1080p runs at every gate and its findings are recorded**, and becomes
  the bar in phase 2. At 1080p the engine tiles the top-bar art, the one output of `0x467D70`
  not yet characterised.
- **Classic++**: dump the UI atlas twin like the sprite twins and hold it against G15-0's
  offline output for the same frames with the `featdiff` machinery; the Q2 bar unchanged (max 1
  level, under 0.01 % of far-band bytes).
- **Nothing moved elsewhere**: the `tascene ab` parity md5 (the world is untouched); the
  `200v200`, `fx-mix` and parity-fixture frame rates within half a frame; the ring's peak
  occupancy and overflow count logged; G13m's cursor measure re-run once after the composite
  change; and **the parity md5 with `tagpu_gui.on` absent equals main's at every landing** — the
  proof that the seam is one seam.
- **Multiplayer**: the detours are observers on draw leaves and never touch sim state, so the
  standing MP check is a two-instance game that finishes, not a replay byte-diff.

---

## 4. The module

| file | role | when |
|---|---|---|
| `tagpu_gui.h` | the only public contract: install, per-present step, GL reset, and `tagpu_gui_layer()` handing the composite its two textures and one flag | phase 1 |
| `tagpu_gui_surf.c` | the twins, the queue replay, seed, **the UI GAF atlas** (an instance of the shared `TAGPU_GAFATLAS`), the layer draw, the trigger poll, `strict` | phase 1 — **built, G15b** |
| `tagpu_gui_int.h` | the SPSC queue between the two halves: 65 536 ops and a 16 MB arena, private to the family | phase 1 — **built, G15b** |
| `tagpu_gui_hook.c` | the observer detours, the census, the publisher | phase 1 — **built, G15a + G15b** |
| `tagpu_gui_art.c` | the sequence-name registry and the `uirestore` policy — G15e; the atlas the plan put here lives in `tagpu_gui_surf.c` as built, and G15e may split it back out | G15e |
| `tagpu_gui_snap.c` | today's `tagpu_ui.c`, moved in unchanged; its `tagpu_ui.trigger`/`.json` names stay so tacli is untouched | phase 1 |
| `tagpu_gui_cursor.c` | our cursor | phase 2 |
| `tagpu_gui_scale.c` | the scale factor, logical-to-device mapping | phase 2 |
| `tagpu_gui_menu.c` | the §2.10 Options menu, drawn as **a surface of ours in the same registry** with no engine surface behind it, through the same ops | after phase 1 |

Shared infrastructure stays shared and outside the module: `tagpu_gaf.c`, `tagpu_detour.c`,
`tagpu_text.c` (world labels use it too), the restorer job pool. The module never reads world
state; its one outward dependency is the true viewport rect for the transient clear, from
`tagpu_vpwide_true_rect()`, like every other pass. The composite in `tagpu_native.c` calls
`tagpu_gui_layer()` and nothing else.

---

## 5. The gates

One finished unit each; the engine's surface is the oracle throughout.

| gate | builds | exit (measured) | kill / pivot |
|---|---|---|---|
| **G15-0** offline art spike — **done: run 2026-09-07, verdict 2026-09-08, §8** | `tools/undither/uiart.py`, seven contact sheets, the consistency table | **met**: every class passed, order buttons ruled in, default = `all` minus `cursor*`, `pathicon`, under 12×12 | no class passes → Classic++ UI dropped from phase 1, UI-aware fine-tune filed as a candidate; the Classic half unaffected |
| **G15a** census — **done 2026-09-07, §9** | observer detours on every pixel-writing leaf, the flip marker, the whole-surface diff, the allocator/free pair, the walk script (`tools/uiwalk.py`); **no drawing** | writer table in [the engine map](exe-reverse-engineering.html) (leaf, convention, call sites, surfaces written); unexplained pixels under 1 % on every inventory screen or every remaining writer named; the minimap's draw path located; the two stale claims in `ui-markers.md` §4 and `frame-composition.md` §1 corrected | a core screen with a large untraceable writer → that surface is seed-only, the plan proceeds |
| **G15b** twins, in game, Classic — **done 2026-09-07, §10** | the `tagpu_gui_*` module: registry, seed, queue, replay, the three op kinds, the index twin (the colour twin is G15e's), the composite seam, `strict`, trigger, tacli verb, the default arm set, the transient viewport clear | side panel, build pages, top and bottom bars at 1024×768: 0 differing pixels outside the cursor, 0 holes; fps fixtures within half a frame; ring peak and overflows logged; parity md5 unchanged with the trigger absent; 1080p run and recorded | replay cannot hold 60 fps at `200v200` → collapse identical per-frame ops before anything else (**it happened, for a different reason — §10**) |
| **G15c** the rest of the frame — **done 2026-09-07, §11** | chat, the F4 and hold-SPACE box, the option screens over the viewport, the `+clock` and `+bps` strings, the minimap's picture, dots and box, the mode-switch panel painter at 1080p; the `LIGHTBAR` wipe located but not reached (§11); nothing new in the DLL — the walk gained a side switch, nineteen stops, the in-viewport measure and a bracketed shot, plus the CORE fixture | whole in-game inventory clean under `strict`, ARM and CORE, at 1024×768 and 1920×1080: 32 of 32 stops at 0/0/0 in all four runs; dialogs over the viewport exact at 0.5× and 2× | — |
| **G15d** the shell — **done 2026-09-07, §12** | the publisher's stall guard and the render thread's skip-to-reset across the context switch (the storm the first cycle showed: 38 overflows, 39 resets, 705 lost sprites per return), the main offscreen's direct free handled, every reset logged with its reason, the twin resolved through the **presented** palette (gamma-scaled by the engine on its way to DirectDraw, never in `main+0x143A7`), the loading screen captured at its one present, the walk's `--cycles` | shell inventory clean under `strict` at 640×480 on every visit; three entry/exit cycles with twins and atlas back to the same counts, one reset per switch, 0 overflows, 0 lost; the loading screen exact; `+gamma 15` presented right while every `+0x143A7` reader is wrong | — |
| **G15e** Classic++ UI — **built 2026-09-08, §14; the Q2 diff still owed** | the per-surface colour twin (MRT with the index), the copy carrying both channels, the palette-validity rule with its re-arm, the atlas's restored twin at priority 4, the 12-px restore floor, `norestore` | sheets judged by the owner ✓ (2026-09-08, every class passes); fps unchanged ✓ (60.0); a fade shows indexed art, never wrong colour ✓ (`+gamma 15`: 235 entries differ, colour dropped, re-armed, valid again); **Q2 bar against G15-0's offline output: NOT RUN** | per-class exclusion per G15-0 |

**Landings and reviews** per the house rule: G15b and G15c land separately — G15b is the
composite seam and the module skeleton, which other worktrees will merge under, so it lands
small and early. G15a and G15b review at `high` (new byte patches), G15c and G15d at `high` if
they add patches else `medium`, G15e at `medium` (a new GL object and a shader branch, no
patch), G15-0 skips the review (tools and docs). Each landing's documentation pass adds its
addresses to the engine map and its rows to [GPU status](gpu-status.html) §2, and this page's
gate table gets its status.

**Unlocked, not scheduled here**: the §2.10 Options menu as a surface of ours. Phase 2's cursor,
shell scaling and in-game scaling are now designed and gated — **§13.9**.

---

## 6. What phase 2 must find intact

- ~~The twin is `surface × k` with `k` a parameter~~ — **§13.2: the twin stays 1:1 and the sharp
  layer carries `k`.** Every op is still recorded in logical coordinates, which is what mattered.
- Sprite ops keep the frame identity, so the colour twin can be filtered when scaled; the UI
  atlas can take the unit atlas's `pad/align/mip` (4, 4, 2) without a model change.
- Pixel ops are indices with coverage, scaled by nearest; a string op is an addition, not a
  rewrite.
- The cursor is outside the twins.
- The module reads no world state, so the logical/device split lands in `tagpu_gui_scale.c` and
  the composite, nowhere else.

---

## 7. Open  [OPEN]

- **The minimap's radar arcs are correct by accident, and one plausible change breaks them**
  [VERIFIED 2026-09-08]. `0x4C0070` (the coverage arcs) and `DrawPoint 0x4BEE60` write the
  composite `main+0x142DB` and are **not** in `LEAVES[]` — nothing observes them. Their pixels
  reach the twin anyway, because the rebuild `0x466DC0` copies the base in first
  (`0x4C6B70([0x142DB],[0x142DF],0,0)`) and that copy's source is written only by `0x466C20`'s
  direct byte writes, so `+0x142DF` is never a destination of an observed op, so it is **never
  seeded**, so the copy degrades to a **pixel op whose bytes are the destination's final state at
  publish time** — arcs included (§10, "a pixel op is the final state of its box"). *The
  precondition is that `+0x142DF` never becomes seeded.* Observe anything that writes it, or make
  copy sources seed on demand (the obvious cure for the shell's 300 KB background pixel ops), and
  the base copy becomes a true twin→twin `PK_COPY` — **and the arcs and points vanish from the
  minimap**, in a build whose only change was elsewhere. Note also that the census cannot vouch
  for them: their pixels lie inside the copy's box, so they count as explained either way. Two
  entries in `LEAVES[]` would make the correctness deliberate; until then this is a trap, not a
  bug.
- ~~Whether the shell changes the palette~~ — **G15d**: it does not, and `guipal` is the GUI's
  *logical* palette (256 entries matched into `main+0xDCB`), never the live table; the corpus's
  "menu fades" are the campaign glamour screen's (`0x41DA60`, `0x41DFC0`, `0x41E270`), which no
  skirmish reaches. What *does* differ is the screen's palette: the engine scales every palette
  it sets by the Gamma option on the way to DirectDraw and never scales `+0x143A7` — the twin
  follows the presented one since G15d; **the world passes still read `+0x143A7` and are wrong
  by the factor at any Gamma but 12** (engine map, "The palette the screen is presented with").
- **The fork keeps the game-sized window on the second return to the shell** (G15d, MEASURED at
  1920×1080: the first return gives a 640×480 window, the second and third a 1912×1040 client
  with the 640×480 shell scaled into it) — cnc-ddraw's `WM_SIZE` → `dd_SetDisplayMode(0,0,0,0)`
  path against the engine's `SetWindowPos(640,480)` at `0x491AFB`, not the layer (which draws
  through the same viewport transform as the engine's frame). Those stops are reported "not
  1:1" and not measured. Whose, and why only from the second time, is open.
- The publisher's cadence is the census's 5 ms, three times the present rate in game, and every
  batch re-reads the box bytes of every non-sprite op (~150 KB): a cadence tied to the present
  would cut the arena traffic threefold and the stall guard's high-water marks with it.
- ~~Where the minimap picture and radar dots are drawn~~ — **G15a**: `DrawMinimap 0x466B00`
  at `0x46961F`, a copy of `main+0x142DB` plus the view box; the dots are drawn into that
  composite by `0x466DC0` from the sim side (engine map, "The minimap, located").
- ~~The `OFFSCREEN` free routine~~ — **G15a**: `0x4C6AC0`; the GUI's surfaces come from
  `0x4C69F0` with the screen's name as tag, plus a `"SAVE UNDER"` snapshot each.
- ~~What `GUI_BlitToFramebuffer 0x4B0230` and the `id 10` handler `0x4A4C90` do~~ — **G15a**:
  read (engine map, the handler table); both draw through observed leaves.
- **Two engine surfaces are written by a path no leaf observes** — the startup
  `"OFFSCREEN" 640×480`, still written in game at in-game screen builds (rows 226–479, up to
  185 942 bytes), and `"FLIPSURFACE" 128×352`, filled whole when `PREFS` opens. Neither is
  presented; whatever reaches the frame from them goes through an observed copy, so the twin
  layer treats a copy from a dirty source as a pixel op (§3.6). The writer of each is still
  to be named.
- The top-bar art at 1920×1080 (tiled by `0x467D70`): fine at 1024×768, uncharacterised wider
  — though G15b's 1080p walk found it drawn pixel-exact by the twin, so whatever the tiler does
  is a plain GAF blit the sprite op reproduces.
- `scenarios/tascene-parity.json` is static only at its own 1024×768: at 1920×1080 a geo-vent
  smoke puff is in view (§10), so a whole-frame md5 there is meaningless and every 1080p parity
  measure has to name the puff's box. A 1080p-static fixture, or an exclusion in `tascene ab`,
  is owed to whichever gate first needs a bare number at 1080p.
- Whether the shell draws anything outside the gadget dispatcher besides the loading screen and
  Smacker frames (which reach the frame through the fallback and are excluded from `strict`).

---

## Appendix — addresses this plan names

All already in [the engine map](exe-reverse-engineering.html) or the pages cited, with the status
they carry there; G15a adds the new ones.

| VA | name | status |
|---|---|---|
| `0x46A303` | `DrawGameScreen`'s call into the GUI draw, `0x4AB170(main+0x519, &ctx, main+0x37E27)` | VERIFIED 2026-09-06 |
| `0x4AB170` | GUI draw thunk `(GUIInfo*, OFFSCREEN*, RECT*)`, `ret 0xC`; loads `gi+0x18` | VERIFIED 2026-09-06 |
| `0x4AB0B0` | per-screen draw: recurse `+0x00`, blit `panel+0xBC` via `0x4C6B70` when `+0x14 == 1` or `0x4B67D0(&rect, vp)` | VERIFIED 2026-09-06 |
| `0x4B67D0` | rect-overlap test | [INFERRED] name, call shape verified |
| `0x4C6B70` | surface→surface blit `stdcall(dst, src, x, y)`, `ret 0x10`, over `0x4CBBE0` | VERIFIED (ui-markers §6.4 session) |
| `0x4CBBE0` | `CopyScreenContext`, raw 8bpp clipped rect copy | VERIFIED |
| `0x4C63A0` | `FlipOffscreenToPrimary`, 44 callers | VERIFIED |
| `0x4C69F0` | `SurfaceCreateNamed(tag, w, h)`, `ret 0xC`; pixels inline at `+0x30`; the tag names the surface | VERIFIED |
| `0x4C6AC0` | `SurfaceFree(surface)`, `ret 4` | VERIFIED 2026-09-07 |
| `0x4C6890` | `SurfaceFill(surface, colour)`, `ret 8` | VERIFIED 2026-09-07 |
| `0x4C5E70` | `GetContext(out)`: NULL-context path, arm 1 = `*(globals+0xBC)` | VERIFIED 2026-09-07 |
| `*(0x51FBD0)+0xBC` / `+0xDC` | the system back buffer every flip presents / its valid flag | VERIFIED 2026-09-07 |
| `0x4C7580` | textured-triangle stamp `(ctx, src, xy[6], uv[6])` | VERIFIED; args MEASURED 2026-09-07 |
| `0x4C6D20` | descriptor blit `(ctx, desc, src, dst)`, `ret 0x10` | VERIFIED 2026-09-07 |
| `0x4BF4D0` | framed box `(ctx, RECT*, colour)`, `ret 0xC` — the F4 popup's border | VERIFIED 2026-09-07 |
| `0x4BF7B0` | the focus rectangle, `(ctx, RECT*, colour)` | VERIFIED 2026-09-07 |
| `0x4A81E0` | `GUI_StageUpdateDraw(gi, flags)`, `ret 8`; flags `1` build, `2` teardown, `0x40` redraw | VERIFIED 2026-09-07 |
| `0x466B00` | `DrawMinimap(ctx)`, `ret 4`, one caller `0x46961F` | VERIFIED 2026-09-07 |
| `0x4A9176` / `0x4A962C` | gadget type switch / 13-entry table | VERIFIED (gui-gadgets §3) |
| `0x4A5F40`, `0x4A1B40`, `0x4A4D70`, `0x4A3EF0`, `0x4A56B0`, `0x4A4980`, `0x4B0230`, `0x4A4660` | the per-type handlers, `ret 8` (`0x4B0230` and `0x4A4C90` `ret 0xC`), all drawing into `[panel+0xBC]` | VERIFIED 2026-09-07 |
| `0x4A4C90` | `id 10` handler `(gi, idx, flags)`: two lines | VERIFIED 2026-09-07 |
| `0x4A5E50` | the `id 12` handler (the type table's entry 11; `gui-gadgets.md` said `0x4A5F40`) | VERIFIED 2026-09-07 |
| `0x4B7F90` | `CopyGafToContext stdcall(OFFSCREEN*, GAFFrame*, x, y)` | VERIFIED |
| `0x4B8500` | `AlphaCompsteBuf2OFFScreen`, the shaded variant | VERIFIED |
| `0x4B8150` | clipped GAF-descriptor blit, raw/RLE | VERIFIED |
| `0x4C6E70` | unclipped 32×32 tile copy | VERIFIED |
| `0x4B8D40` / `0x4B8DA0` | GAF sequence lookup by name | VERIFIED |
| `0x4CCF60` | glyph blitter, cdecl 9 args, destination pointer, no clip | VERIFIED (G13p) |
| `0x4C14F0` / `0x4C1420` / `0x4C13A0` | `DrawTextCustomFont` / `SetFont` / `SetTextColors` | VERIFIED |
| `0x4BE950` / `0x4BF6F0` / `0x4BF8C0` | `DrawLine` / `DrawBar` / `DrawTranspRectangle` → `0x4CC7AB`, `0x4CCDEA` | VERIFIED |
| `0x467D70` | HUD panel painter, called at `0x49842A` after a mode switch | VERIFIED (resolution §3) |
| `0x466B70` | minimap view box → `main+0x142CB` | VERIFIED |
| `0x4C2870`, `0x4C67C0` | cursor blits: end of `DrawGameScreen` (`0x46A3C7`), and twice inside the flip | VERIFIED |
| `0x4981C9..0x498237` | the viewport rect writer: `left=128`, `top=32`, `right=W−1`, `bottom=H−33` | VERIFIED |
| `0x498025..0x498108`, `0x491ADC` | the shell forced to 640×480 | VERIFIED |
| `0x498109` | `palettes/guipal` loaded at game entry | VERIFIED |
| `main+0x37E1B` / `main+0x37E27` | the main `OFFSCREEN` / the viewport rect | VERIFIED |
| `main+0x519`, `+0x531` | `GUIInfo`, `TheActive_GUIMEM` | VERIFIED |
| `main+0x1426B` | `TED_GENERATED_PIC`, the minimap picture | CORPUS name, address used by `tagpu_zoom.c` |
| `main+0x143A7` / `main+0xDCB` | the live RGB palette / the 16-entry GUI colour LUT | VERIFIED |
| `main+0x3907F..0x3908B` | `Palette`/`currentPalette`/`desiredPalette`/`FadeTable` | CORPUS; "menu fades" [INFERRED] |

---

## 8. G15-0 — the offline art spike  [MEASURED 2026-09-07]

`tools/undither/uiart.py` pulls the UI art out of the archives with no game running, restores
it with the shipped `full` model through the unditherer CLI (`--preset learned --report
--consistency`), and lays original beside restored on one contact sheet per class, worst
dither-consistency first. 224 frames in seven classes; the sheets are in
`assets/shots/uiart/`, the per-frame numbers in `uiart-report.json` beside them. **The owner's
verdict came 2026-09-08: every class passes, the order buttons included** (below). What
follows is the run, the reading it produced, and the ruling.

Two facts the run established before it restored anything:

- **The shell runs on `palette.pal`, not `guipal.pal`.** Every shell PCX carries a palette that
  differs from `palette.pal` in 4–32 reserved entries and from `guipal.pal` in 254–256, and
  `guipal.pal` differs from `palette.pal` in 254 of 256 entries. So `guipal` is the loading
  screen's palette (loaded at `0x498109`), and a GAF frame drawn in the shell resolves through
  the same palette as in the game — which is what §3.4's index twin assumed **[MEASURED]**.
- **The six `bitmaps/*gui*tile.pcx` files are not the panel art.** Each is a 640×480 canvas that
  is 98.6 % one fill index around a single 129×33 strip — the piece `0x467D70` tiles along a
  bar. The side panel proper is `ARMPAN`/`CORPAN` in `anims/commongui.gaf` (128×352), and each
  screen's buttons are in `anims/<screen>.gaf` (118 of 188 screens have one; `MAINMENU`'s
  buttons are in `oldmain.gaf`). The spike marks a PCX sheet's fill as its colour key so the
  restorer inpaints it, as the game's GLSL path inpaints keyed texels; fed as content it produced
  a halo along every panel edge, which is the first run's lesson and not a property of the art.

| class | what | frames | q median | shift median | consistency match min / median | hist median |
|---|---|---|---|---|---|---|
| bg | shell backgrounds, dialogs, sprite sheets (`bitmaps/*.pcx` ≥ 320 wide) | 73 | 2.9 | 0.20 | 0.438 / 0.684 | 0.885 |
| gaf | `commongui.gaf` panels and order buttons, the shell and in-game screens' `anims/<screen>.gaf` | 92 | 33.3 | 1.18 | 0.313 / 0.626 | 0.779 |
| unitpics | a sample of `unitpics/*.pcx` (24 of 282) | 24 | 40.2 | 0.11 | 0.422 / 0.701 | 0.891 |
| hud | the six bar strips, cropped | 6 | 20.9 | 0.07 | 0.302 / 0.451 | 0.733 |
| small | `bitmaps/*.pcx` under 320 wide (logos, `gamesettings`) | 6 | 5.8 | 0.90 | 0.374 / 0.631 | 0.792 |
| cursors | `anims/cursors.gaf`, the expected exclusion | 20 | 11.0 | 1.47 | 0.331 / 0.462 | 0.594 |
| screens | three captured 640×480 menu surfaces, text included — the cautionary column | 3 | 11.6 | 0.09 | 0.548 / 0.616 | 0.874 |

`q` is the unditherer's measured dither amplitude (0 = it found no dither), `shift` the mean
colour shift in levels, `match` the fraction of pixels that land back on their source index
when the output is Floyd–Steinberg re-quantised, `hist` the histogram overlap. The scores are
not comparable across classes: a flat-colour logo re-dithers to itself (high match, nothing
restored), a 10×20 cursor cannot (low match, nothing to restore either). They rank frames
*within* a class for the sheet; the sheet is the evidence.

<figure style="margin:0"><img src="assets/shots/uiart/uiart-unitpics.webp" alt="G15-0: unit pictures, original beside restored at 3x"><figcaption><code>unitpics</code>, original left, restored right, 3×. The model's home ground — a rendered model on dithered terrain — and it shows: the terrain dither and the sky banding resolve, the model's edges hold (edge retention 0.99–1.0 on every frame), mean shift 0.08–0.43 levels.</figcaption></figure>

<figure style="margin:0"><img src="assets/shots/uiart/uiart-gaf-panels.webp" alt="G15-0: the side panels and the shell panels, a 96x96 window at 3x and a micro crop at 8x"><figcaption><code>ARMPAN</code>, <code>ARMPAN2</code>, <code>FRONTPAN</code>, <code>CORPAN</code> and the option panels: the busiest 96×96 window at 3×, then a 24×18 micro crop at 8×. The dark panel grain — a two-index dither in the original — becomes a smooth dark surface; the bevel lines, grooves and <code>FRONTPAN</code>'s diagonal stripes survive at full contrast.</figcaption></figure>

<figure style="margin:0"><img src="assets/shots/uiart/uiart-screens.webp" alt="G15-0: three captured menu screens with their text, restored whole"><figcaption>The cautionary column: whole captured menus, text included. The button faces come out clean; the glyphs come out softened, the dark outline bleeding a fraction of a pixel into the light stroke (the micro crops). This is why §3.6 keeps text as captured indices the restorer never sees.</figcaption></figure>

The other sheets: [`uiart-bg.webp`](assets/shots/uiart/uiart-bg.webp) (the twelve lowest-scoring
backgrounds, a 160×120 window at 3×), [`uiart-gaf.webp`](assets/shots/uiart/uiart-gaf.webp) (the
60 lowest-scoring GAF frames whole, 3×), [`uiart-hud.webp`](assets/shots/uiart/uiart-hud.webp),
[`uiart-small.webp`](assets/shots/uiart/uiart-small.webp),
[`uiart-cursors.webp`](assets/shots/uiart/uiart-cursors.webp).

**The reading, and the owner's ruling on it [DECIDED 2026-09-08]:**

- **`unitpics` and the panels pass on sight.** The build icons are exactly what the model was
  trained on; the panels lose their grain and keep their geometry.
- **The order buttons (`ATTACK`, `PATROL`, `REPAIR`…) were the one judgement call, and they are
  RULED IN [DECIDED 2026-09-08]** — the rounding is accepted, no name-glob exclusion. Their labels
  are baked into the GAF frame, not drawn as text, so they *are* fed to the model. On the sheet
  the metal face smooths and the letters stay legible with slightly rounded corners (mean shift
  2.5–3.7 levels, the highest in the class). Whether that softening is acceptable, or those
  frames join the exclude list by name, is the owner's call; both are one glob.
- **Cursors change little and gain nothing** — hard-edged 10–33 px sprites under a 25-px
  receptive field; `cursormove` shifts 4.1 levels for no visible reason to. Excluded as planned.
- **Backgrounds are mixed by kind, not by quality**: painted scenes (`mission02win`, the water)
  and the metal title art restore cleanly; the flat logos (`armbkg`, `corebkg`) barely change;
  the sprite sheets (`grommets`, `loadbar`, `stagebuttons`) are keyed fill around small pieces
  and are judged through the pieces, which look like the GAF buttons above.
- **Text softens** (the `screens` column): confirmed, and already designed out.

**The default for `uirestore` [DECIDED 2026-09-08]**: `all` minus `cursor*`, `pathicon` and
anything under 12×12. The order-button sequences are **not** excluded — the owner ruled the label
rounding acceptable on the sheet. The two exclusions that remain are **not** aesthetic judgements
and survive the "they all look good" verdict on purpose: below the model's 25-px receptive field
there is nothing to restore, so the work produces approximately its input, and one cursor frame
(`cursormove`) shifts 4.1 levels with nothing visible to show for it — a change with no benefit is
an unforced risk, not a win. Keeping cursors indexed also keeps them byte-identical to the
engine's, which §13.5 wants when the cursor becomes ours at a fixed 1× device size. Nothing in the
run argues for dropping the Classic++ half: the kill rule of §3.9 is not triggered.

**What the ruling does not cover.** The spike restored frames *in isolation*. The halo the game's
GLSL path produces at a keyed edge (§4c's near band, 0.41 levels on features) has never been
measured on UI frames, and panels and buttons are full of keyed edges. **The exclude list may
still grow at G15e**, on that evidence rather than on the sheets'.

**What the spike did not do.** It restored frames in isolation, as the game will; it did not
measure the halo the *game* path produces at a keyed edge (§4c's near band, 0.41 levels on
features) on UI frames, which G15e's `featdiff`-style check will. The `oldmain.gaf` dissolve
frames (`intro`, `multi`, `exit`) are noise by design and score low for it; they are animation,
not art, and the default policy treats them like any frame.


---

## 9. G15a — the census  [MEASURED 2026-09-07]

**Built** (`tagpu_gui_hook.c`, `tagpu_gui_leaves.h`, `tagpu_gui.h`; `tagpu_detour_observe`
and stub chaining in the shared detour code; `tools/uiwalk.py`): an observer detour on every
function that writes UI pixels — the GAF blits `0x4B7F90`/`0x4B8500`/`0x4B8310`, the descriptor
blit `0x4C6D20`, the textured-triangle stamp `0x4C7580`, the glyph blitter `0x4CCF60`, the line,
bar, hollow-rect, focus-rect and framed-box drawers, `SurfaceFill`, the surface copy
`0x4C6B70`, the allocator (its return hijacked) and the free, `GUI_StageUpdateDraw` as an event —
and on the flip. At every flip, throttled to one census per 5 ms, the presented surface is
diffed against its copy, every recorded op's box is subtracted, and inside the world viewport a
changed pixel that is now the key is the terrain skip's erase; what is left is a writer nobody
named. Every surface an op has named is diffed the same way, seeded at its allocation so a
screen's *build* is diffed and not adopted. Nothing is drawn; the engine's behaviour is
byte-identical with the module armed (every observer calls the original).

**The result: 0 unexplained of 3 710 035 changed pixels on the presented surface across the
inventory** — `MAINMENU`, `SINGLE`, `SKIRMISH`, `SELMAP`, `STARTOPT`, `VISUALS` and back, then
`ARMMAIN2`, `ARMCOM1` and its second page, `ARMOPT`, `PREFS`, `VISUALRT`, the chat (`TALK`), the
F4 popup — at 1024×768 with every world pass armed (`tools/uiwalk.py --inst g15a`). A shell
transition changes the whole 640×480 and is explained by tens of thousands of GAF blits plus a
few thousand lines, rects and copies; an in-game screen build by hundreds to a few thousand.

**What it found, in the order the residuals fell:**

1. **The flip presents `*(globals+0xBC)`**, not `main+0x37E1B` (the same object in game, a
   different one in the shell), and a NULL context draws there too — the first census diffed
   the wrong surface in the shell and every shell change was "unexplained".
2. **Inside the viewport only the key is the erase.** Subtracting the whole viewport hid the
   in-game dialogs, which are drawn over the world: `ARMOPT`, `PREFS`, `VISUALRT`, `TALK`.
3. **The option screens' wide dark backdrop is textured triangles.** `PREFS` left a 149×351
   block with slanted edges right of the 128-px panel; `0x4C7580` takes three screen vertices
   and three texture coordinates, and 13–37 of them per build paint that backdrop.
4. **The F4 popup's border is `0x4BF4D0`**, a three-fill framed box no note named.
5. **The shell flips ~5 000 times a second**, so the census is throttled and its log reports
   window totals rather than single censuses (a changed-but-explained census between two
   quiet ones was invisible until it did).
6. **The startup splash is drawn before the first flip**, so the game thread has to be taken at
   DllMain rather than at the first flip.

**What stays outside the leaves, all of it off the presented surface** (engine map, "What the
census measured"): the PCX backgrounds decoded into their own surfaces by the loader (read only
as copy sources); the `SAVEMOUSE` buffers, written through `0x4CBBE0` directly by cursor code;
and two engine scratch surfaces — the startup `"OFFSCREEN" 640×480`, still written in game at
screen builds, and `"FLIPSURFACE" 128×352`, filled when `PREFS` opens — whose writers are
[OPEN] and whose content only reaches the frame through an observed copy. For the twin layer
this means one rule: **a copy op whose source has no twin, or whose source changed by no
observed op, is replayed as a pixel op from the source's bytes** (§3.5's seed, applied to a
source).

**Two things the gate corrected along the way**: `tacli shot` had been silently broken in game
since the window-title landing (the title's `:` and `|` made an illegal PNG filename;
`screenshot.c` sanitises it now), and three wiki claims — the profiler bars called "side panel /
minimap" in `ui-markers.md` §4, the minimap row of `frame-composition.md` §1, and `id 12`'s
handler in `gui-gadgets.md` §3.

**Cost, measured on the game thread**: one 1024×768 diff (768 KB compare, the changed rows
copied) per census at ≤200 censuses a second; the parity fixture's frame rate did not move
(the walk ran at the fixture's usual 59–60 fps with `native terr feat fx sfx mark order zoom`
armed). The op ring (65 536 entries) never overflowed (`dropped=0` throughout).

**Exit**: the writer table is in the engine map, unexplained is 0 % on every inventory screen,
the minimap's draw path is located, and the three corrections are made. **Not done here**:
1920×1080 (the census is resolution-independent by construction, but the run is owed at every
gate); the census in the multiplayer lobby screens, out of the inventory by decision.

---

## 10. G15b — the twins, in game, Classic  [MEASURED 2026-09-07]

**Built.** The second half of the module, on the render thread, and the publisher that feeds
it: `tagpu_gui_int.h` (the queue), `tagpu_gui_surf.c` (twins, atlas, replay, the layer),
the publisher in `tagpu_gui_hook.c`, `tagpu_gaf_atlas_put`/`_find` in the shared GAF code,
two lines in `tagpu_overlay.c` (the seam and the GL reset), `tacli gui`, and
`tools/uiwalk.py --layer`, the strict walk of §3.11. Nothing is patched beyond G15a's
observers; with the trigger absent the DLL's frame is main's byte for byte (below).

### How it works as built

The census ring of §3.6 is the record. **Inside the flip observer, at the census cadence (at
most one publish per 5 ms), the ring since the last publish becomes queue ops** — the queue is
a lock-free single-producer, single-consumer ring of 65 536 ops with a 16 MB byte arena, the
only thing the two threads share:

| ring op | published as | carries |
|---|---|---|
| a surface first seen since the last reset | **seed** | the surface's bytes, whole, read now |
| `0x4B7F90` of a frame ≤ 512 px, no sub-frames | **sprite** | the frame identity `(header, pixel pointer)`, the destination, the colour key; on first sight the frame's pixels, decoded on the game thread |
| `0x4C6B70` from a twinned source | **copy** | source, box, source top-left |
| a flip after which `terrown`'s fill sequence advanced | **clear** | the true viewport rect |
| everything else — text, lines, rects, fills, the descriptor blit, the textured triangles, the shaded and sub-frame GAF variants, a copy from an untwinned source | **pixels** | the box's bytes **as they stand at publish time** |

Three things the run settled, none of them in the plan:

- **A pixel op is the final state of its box, not the op's own output.** Reading the bytes at
  publish time — the frame is complete, the flip is running — makes the twin converge on the
  engine's surface whatever order the writers ran in, and retires the CPU shadow-write of §3.6
  for the non-sprite ops: the residual is simply the bytes.
- **Sprite pixels cross the thread boundary in the op** (§3.5 said so, and it bit at once):
  the shell frees a popped screen's art while the render thread is still behind, so the atlas
  is fed from the arena by `tagpu_gaf_atlas_put`, never from the frame pointer.
- **The shell redraws every gadget on every flip** — ~41 ops at ~12 000 flips a second on
  `MAINMENU`, the same with the layer on or off — and every one of those redraws is identical.
  Unthrottled that flooded the queue into a reseed storm (10 000 resets in two minutes); the
  gate's kill rule ("collapse identical per-frame ops") was needed on day one, for the shell,
  not for `200v200`. Within one published batch, identical ops keep only their last
  occurrence, which is the one whose position in the order matters; a replayed op is
  idempotent, so the final state is unchanged. After it: resets 1 in the shell, overflows 0.

On the render thread, inside `tagpu_overlay_draw` right after `tagpu_native_frame`: poll the
trigger (500 ms), drain (up to 20 000 ops a present — a burst is many engine flips), draw.
Every seeded surface has a **twin**: an `RG8` texture its size (R = the palette index, G =
coverage) behind an FBO, 1:1, `NEAREST`. Sprites are quads from a `TAGPU_GAFATLAS` of UI
frames (2048², `pad 0 align 0 mip 0`, the key discarded per fragment), copies are quads
sampling the source twin at an offset, seeds and pixels are `glTexSubImage2D`, clears are
scissored `glClear`s to coverage 0. **The seam is one draw**: the presented surface's twin over
the whole frame into the overlay's target, blending and depth off, `discard` where coverage is
0 — the native composite's key rule beneath is untouched, so the engine's pixels remain the
fallback everywhere a twin has nothing, which is §3.3's three layers without a third texture
in the composite. The index resolves through the live palette (`main+0x143A7`, re-uploaded on
change). The cursor's rect (the mouse object's sprite record and last-drawn position) is left
to the engine's frame and exempted from `strict`; it was a 10×20 diff over the panel until it
was. Excluded at the source, on the return address: the unit composite blit, the cursor code,
the flip's own blits ([engine map](exe-reverse-engineering.html), "What the twin layer
excludes, tests and reads").

**Fresh starts**: the trigger reappearing, a GL context change, a queue or arena overflow, a
sprite whose bytes never arrived, a copy from an untwinned source — all raise one flag, the
next publish sends a reset and re-seeds every surface from the engine's bytes. Three per launch
is the normal count (the arm, the shell→game context switch, the game's mode switch).

### What the walk found

The strict walk was the gate's instrument and it found the one real bug: `op_add()` returned
early on a fully clipped box or a NULL surface **without clearing "the op just recorded"**, so
the caller then wrote its frame identity — or a copy's source and offset — into the *previous*
op. Symptom at both resolutions: the last letter of `ARMOPT`'s "Exit" label missing in the
twin (67 px, a glyph whose frame had become the fully clipped blit that followed it), and at
1920×1080, after `ARMOPT` closed, the whole side panel wrong (43 602 px: the `SAVE UNDER`
restore copy carrying the next clipped copy's source offset). One line fixed both.

### Measured

`tools/uiwalk.py --layer` (`strict`, every world pass armed), the engine's surface against our
GL frame at every stop, differing pixels outside the world viewport in game with the cursor
rect excluded, and magenta holes:

| stop | 1024×768 | 1920×1080 |
|---|---|---|
| `MAINMENU` (4 visits) | 183–192 / 0 | 184–188 / 0 |
| `SINGLE`, `SKIRMISH`, `SELMAP`, `STARTOPT`, `VISUALS`, and back (9 stops) | 0 / 0 | 0 / 0 |
| `ARMMAIN2`, `ARMCOM1`, `ARMCOM2`, back | 0 / 0 | 0 / 0 |
| `ARMOPT`, `PREFS`, `VISUALRT`, back, back | 0 / 0 | 0 / 0 |
| chat, F4, F4 closed | 0 / 0 | 0 / 0 |

`MAINMENU`'s differing pixels are its sparkle animation between the two shots — single
scattered pixels in the sky, none on a gadget (the diff image is in the walk's output);
GL-against-GL a second apart on a static screen differs by 0–1 px. Resets 3 per run, overflows
0, `lost` 0, the atlas at 123 of 4 096 entries after the whole inventory, twins 2–10 live.

**Parity with the trigger absent, and with the layer on.** The parity fixture at 1024×768
(`scenarios/tascene-parity.json`, every world pass armed, the eye pinned), `glshot` md5:

| build | `tagpu_gui.on` | md5 |
|---|---|---|
| main (`2b83b12`) | — | `568cc55c4301ab88f166282f969e18b9` |
| this branch | absent | `568cc55c4301ab88f166282f969e18b9` |
| this branch | present, layer on (fallback mode) | `568cc55c4301ab88f166282f969e18b9` |

The third row is the gate's real proof: with the layer drawing the panel, the bars and the
resource text, the presented frame is main's byte for byte.

**At 1920×1080 the same fixture is not static**, so a whole-frame md5 is not a measure there:
the wider view brings a geothermal vent's smoke into frame at screen `(1523..1563, 420..459)`
(world x ≈ 3292, off-screen at 1024×768), and main's DLL differs from *itself* across two
launches by 606 px, all inside that puff. So the 1080p test is a pixel diff with that box
named — every pair differs **only inside it**:

| pair (1920×1080, `glshot` ~10 s after load) | differing px | outside the smoke box |
|---|---|---|
| main, launch 1 vs launch 2 | 606 | 0 |
| main vs this branch, trigger absent | 482 | 0 |
| this branch, trigger absent vs layer on | 503 | 0 |
| main vs layer on | 689 | 0 |
| main, the same launch 5 s apart | 358 | 1 (at the cursor, `(960,540)`) |

A future 1080p md5 wants a fixture with the eye a few hundred world units left of this one,
or the puff's box excluded; neither is done here.

**Frame rates**, from the arrival cadence of the overlay's roster line (every 30 presented
frames, timed from outside the process — the one meter that works for main's DLL too), 40
intervals each, the fixture fresh after `scenario load`, the same three other instances idle
in the background for every row:

| fixture | resolution | main (`2b83b12`) | this branch, trigger absent | layer on | armed, `off` |
|---|---|---|---|---|---|
| `tascene-parity` | 1024×768 | 60.00 | 60.00 | 60.00 | 60.00 |
| `200v200` | 1024×768 | 59.81 | — | 59.81 | 59.85 |
| `fx-mix` | 1024×768 | 60.00 | — | 59.99 | 59.99 |
| `tascene-parity` | 1920×1080 | 60.01 | 60.00 | 59.99 | 60.00 |
| `200v200` | 1920×1080 | 59.78 | — | 59.77 | 59.86 |
| `fx-mix` | 1920×1080 | 60.00 | — | 59.99 | 60.00 |

Within 0.1 fps of main everywhere, at both resolutions — the layer's cost is below the
meter's resolution at the 60 fps cap. (The 1080p walk's own heartbeat had read 28 fps while
the two walks ran concurrently beside three other instances; on a lone instance it is 60.)
The heartbeat's queue figures on the parity fixture at 1024×768 after 20 s: 2 twins, 14
seeds, 154 575 sprites, 2 002 copies, 20 729 pixel ops, 328 547 clears, 24 atlas entries,
0 lost, 0 overflows.

### After the review

Two Opus reviewers at `high` on the landing diff, fourteen findings between them, eleven
distinct; acted on eight, all re-verified against the code or the disassembly first:

- **`census_surface` could write past the mask** when a surface was freed and another
  allocated over the same bytes with a smaller size inside one 5 ms window — the ring still
  held the old surface's boxes. `SurfaceFree` now zeroes the ring's ops on the freed base and
  the subtract loop clamps to the surface as it is now. (Both reviewers; the one real memory
  bug.)
- **A publish that hit a box no longer inside its surface stopped the batch silently**, leaving
  the twin stale until something unrelated redrew; it now raises the reseed flag like every
  other early exit. A surface re-registered at a new size also re-makes its twin.
- **Dedup versus a copy**: an earlier duplicate of a write is now dropped only when no
  `0x4C6B70` reading its surface lies between the two, or one follows the survivor (the copy
  then reads the final state either way). The engine's per-flip order — redraws, then the copy,
  then the flip — never produced the unsafe shape, but nothing enforced it. The first fix tried
  was a per-surface epoch bumped by every copy: it re-created the reseed storm (2 749 resets in
  one walk), because the shell copies its panel to the frame on every flip, so every flip's
  identical redraws became distinct — the rule above keeps them collapsing.
- **The glyph observer measured past `'\n'`**, where `0x4CCF60` stops (`0x4CCFA0`); the
  engine map said so and the code did not.
- **The sprite identity was two addresses** the shell reuses after freeing a popped screen's
  art; it now carries a hash of the plane's first bytes, read at publish time under the same
  guard as the first-sight decode.
- **The observer stubs did not preserve EFLAGS** around `before` and `after`; no engine caller
  of the observed functions reads flags after the call (checked at every `call 0x4C69F0` and
  `call 0x4C63A0`), but "byte-identical" now holds for the flags too.
- **`tacli gui <name>` created a gamedir on a bare query**; only a state change does now.
- **GL bindings** are dropped after the drain whether or not the layer drew.

Rejected or accepted as designed: a `ret` to 0 if an observer's return hijack is ever
unpaired (only an exception unwind through the flip or the allocator does that, after which the
engine's own handler exits); the byte arena stranding `[aTail, end)` after a wrap until the
consumer catches up (a spurious reseed at worst, the ring's design). Corrected in the notes:
the flip `0x4C63A0` has three exits, not one (`0x4C6669`, `0x4C668F`, `0x4C67BA`), and a
`0x4CBBE0` at `0x4C6769` the table had missed. The fixes were verified by re-running the
1024×768 walk and the parity fixture with the layer on (below the walk tables' numbers stand;
the smoke test's frame differs from the reference by the one cursor pixel main itself flickers).

### Not closed here

- The shell is measured clean at every stop but its entry/exit cycles (twin and atlas counts
  flat across three context switches, the loading screen, the palette) are G15d's; the
  whole-inventory run on CORE and the dialogs over the viewport at 0.5× and 2× were G15c's (§11, done).
- A **clear is published per engine flip** the fill sequence advanced on, and the engine
  free-runs its flip — in a batch of *n* flips only the last clear can matter for the final
  state, so *n−1* of them are wasted scissored clears of the viewport. Cheap at 1024×768;
  see the 1080p row above for whether it is cheap there.
- `tagpu_gui_snap.c` (the gadget snapshot behind `tacli ui`) was moved into the family by G15a
  unchanged; `tagpu_gui_art.c` does not exist yet — the atlas lives in `tagpu_gui_surf.c` (§4).

## 11. G15c — the rest of the in-game frame  [MEASURED 2026-09-07]

**Built: nothing in the DLL.** G15b's layer already carried every writer of the in-game frame;
what G15c owed was to *reach* them and measure. So the landing is the instrument and the
fixture: `tools/uiwalk.py` gained a side switch, twenty more in-game stops, a measure inside the
viewport, and a bracketed shot; `scenarios/tascene-parity-core.json` is the parity fixture with
the sides swapped (CORE human, a `CORCOM` on the anchor, an `ARMSOLAR` as the spare). The
engine facts the stops needed — what gates each HUD extra and what it draws — are in the
[engine map](exe-reverse-engineering.html), "The frame's HUD extras".

### The measure, extended

G15b compared the engine's surface with our frame **outside** the world viewport only, the
world being ours. Inside it the engine still draws the dialogs, the chat, the popups and the
clock over `terrown`'s key fill, and `strict` already defines what counts there: a pixel whose
index is not the key. The walk now applies the same rule — `tacli shot` writes an 8-bit
palette PNG, so the raw index is there to read — and reports, per stop, the non-key pixels the
engine put inside the viewport and how many of them differ from ours. Three things the run
taught about measuring, none about drawing:

- **The cursor rect was never excluded.** `cursor_rect()` parsed `tacli peek`'s lines with a
  prefix the command does not print, returned `None`, and G15b's zeros stood only because the
  pointer sat inside the masked viewport. Read properly (`*0x51FBD0+0x1B6/+0x1BA`, the record
  at `+0x1B2`), padded 8 px because the sprite animates and its record changes frame between
  two shots, it removes the 1-pixel flicker at the screen centre that main itself shows.
- **The clock ticks between the shots.** `+clock` prints the sim tick as h:m:s; the surface and
  the GL frame are a second apart, so the seconds digit differed on every run. The stop is
  taken with the in-game menu open: `ARMOPT` pauses the sim and the clock with it.
- **The engine's frame moves on its own** — a chat line expiring and the log scrolling up, a
  walking unit's minimap dot, the cursor's animation — and a single surface shot cannot tell
  that from a layer error. Every in-game GL shot is now **bracketed** by a surface shot before
  and one after: the diff is taken against the closer one, and the engine's own change between
  the two is its own column. A layer error differs from *both*.

`ARMOPT` turned out not to be "over the viewport" at all: its record is `xpos=0 ypos=128 128×352`,
the side panel's rect, and it pauses the game with `PAUSED` in the middle of the world. The
dialogs that do lie over the world are `PREFS`, `VISUALRT`, the F4 box, the chat and the HUD
strings; the walk zooms those. It opens each at 1× and then writes `tagpu_zoom.txt` for 0.5×
and 2× — at zoom ≠ 1 a click on a dialog inside the viewport is bent by the transform (the
ta-drive skill), and the pixels do not care in which order the two happened. The live zoom is
read back from `mark.on=log`'s periodic line, the file lever logging nothing.

### The stops

After G15b's thirteen (the side's `MAIN2`, `COM1`, `COM2`, back, `ARMOPT`, `PREFS`, `VISUALRT`,
back, back, chat, F4, F4 closed):

| stop | exercises | engine pixels inside the viewport (1024×768) |
|---|---|---|
| `clock` / `clock-off` | `+clock` typed in chat, the menu open (paused) | `Game Time : h:mm:ss` at (130, 723), the `PAUSED` label |
| `bps` / `bps-off` | `+bps` typed | `Receive - … K/s` / `Send - … K/s` at (129, screenH−95) |
| `space-popup` / `-up` | SPACE held with the pointer on the commander (`0x4689C0`) | the Kills/Losses box — F4's twin, 21 382 px both ways |
| `ARMOPT@0.5`, `@2` | the menu (the side panel's rect) with the world at 0.5× and 2× | `PAUSED` |
| `PREFS@1`, `@0.5`, `@2`, back | the options screen over the world at three zooms | ~55 000 px |
| `F4@0.5`, `@2`, `chat@2`, close | the popup and a chat line over the zoomed world | ~21 000 / ~800 px |
| `move`, `move-stop` | the commander ordered across the map and stopped | none (the unit and its bars are ours); the minimap's dot moves ~8 px between the first and last stop |
| `scroll-box` | the eye released, one second of edge scroll, parked | none; the minimap's view box moves a box-width |

### Measured

`tools/uiwalk.py --layer --game-only` (`strict`, every world pass armed, `mark.on=log`), four
runs, each a lone instance, 32 in-game stops each — G15b's thirteen and the nineteen above:

| run | stops fully clean (0 outside / 0 inside / 0 holes) | twins ≤ | atlas ≤ | resets | overflows | fps (heartbeat) |
|---|---|---|---|---|---|---|
| ARM, 1024×768 | **32 of 32** | 10 | 130 | 3 | 0 | 60.0 |
| CORE, 1024×768 | **32 of 32** | 10 | 132 | 3 | 0 | 59.9–60.0 |
| ARM, 1920×1080 | **32 of 32** | 10 | 130 | 3 | 0 | 59.6–60.0 |
| CORE, 1920×1080 | **32 of 32** | 10 | 132 | 3 | 0 | 59.6–60.0 |

"Inside" is every non-key pixel the engine put in the viewport, and there were plenty to compare:
`PAUSED` 2 443–2 750 px (4 133–4 160 with the clock), `PREFS` and `VISUALRT` 54 600–55 700, the F4 and
SPACE box 21 187, the chat 811 (22 352 with the box open at 2×), the `+bps` lines with their chat
echo 1 337. Every one of them matched byte for byte at 1×, 0.5× and 2×; the zoom column read
back 0.5 / 2.0 / 1.0 at the stops that set it. **The bracket earned its place once per run**: at
`clock-off` the chat log scrolled between the shots (846–847 px of the engine's own change) and
the GL frame matched the later surface exactly; every other stop's engine self-change was 0. The
minimap's dot moved 8 px between the first stop and the last (40 px differing between those two
engine shots, all in the minimap) and the view box a box-width after the scroll (52 px), and
both stops were exact against our frame.

**The census, re-run on CORE at 1024×768** (`tools/uiwalk.py --game-only --side core`, no
layer): **1 717 044 pixels changed on the presented surface across the 32 stops, 0 unexplained**.
The residual lines it logged on *other* surfaces were the two known ones — the startup
`OFFSCREEN` 640×480 written whole at in-game screen builds (§7, still open) and a 128×352
buffer the size of the menu's rect, written whole when it opens (its `SAVE UNDER` snapshot
[INFERRED]) — neither presented, both reaching the frame only through observed copies.

**Nothing changed in the DLL, so the trigger-absent parity of §10 stands unmeasured here**: the
binary is G15b's, byte for byte (`git diff main...HEAD -- tagpu` is empty).

### Not closed here

- The `LIGHTBAR` wipe (`0x45FFB0`) is measured through the census and its end state, not
  frame by frame: its opener (`0x460160`) is not
  reached by anything the skirmish inventory does. The census over the menu's opening recorded
  no stamp op at all (`gaf 22 644, line 2 060, rect 1 765, copy 297` in that window; the wipe
  stamps through `0x4C7580`, the census's `scale`), so **Tab does not play it**; its five callers
  sit in the mission-start flow (`0x427459`, `0x427906`) and behind screens named `Options`,
  `Menu` and `SETTINGS` (`0x44444D`, `0x4776D8`) — the multiplayer `TABMENU.GUI`'s buttons by
  their names [INFERRED]. Both leaves it writes through are observed, so the twin will follow
  it when it does run; a frame-by-frame capture of it is owed by whichever gate first reaches a
  campaign start or the multiplayer Tab menu (G15d's context switches are the nearest).
- The status icons at `0x46A107..0x46A1CB` and `0x46A2A8` (`main+0x38A51` bits 0 and 1,
  `main+0x3923B` bits 5 and 6) and the profiler bars (`main+0x38DD5`) were not exercised — no
  play-time control was found that sets them; the debug line at `0x469FD5` is unreachable in
  play (engine map). All of them draw through observed leaves.
- G15b's open items stand: a clear published per engine flip (*n−1* wasted per batch, free at
  60 fps), the 1080p parity fixture not static (the geo-vent puff), `MAX_SURF 24` /
  `MAX_TWINS 32` never approached (twins peak at 10 in the inventory, the atlas at 132 of 4 096).
- The shell's context-switch cycles are G15d's; Classic++ art G15e's.

## 12. G15d — the shell across the context switch, the loading screen, the palette  [MEASURED 2026-09-07]

**Built.** Four things in the DLL, none of them a patch: the publisher stops publishing while
the render thread is dead or crawling, the render thread steps over the stale queue after a
context change, the main offscreen's direct free is handled, and the twin resolves through the
palette the engine's frame is *presented* with. Plus `tools/uiwalk.py --cycles N` — the exit
dialogs, the return, the shell inventory again, the loading screen at its one present, the
fixture re-applied, `+gamma` — and a heartbeat that names every reset. The engine facts are in
the [engine map](exe-reverse-engineering.html), "The palette the screen is presented with, the
way out of a game, and the loading screen".

### What the first cycle showed

`tacli`'s `EXIT → MAINMENU → CHOICE1` from a running game, three times in one process, on
G15c's DLL: every return to the shell cost **38 overflows, 39 resets and 705 lost sprites**,
the same three numbers each time, and every start of the next game 2 more resets. Twins and
atlas came back to the same figures (the atlas is emptied by the context change anyway), so
the gate's literal exit held while the layer thrashed through every switch. Three mechanisms,
read from the code and then from the reset reasons once they were logged:

- **The consumer dies, or crawls.** cnc-ddraw stops its render thread inside every
  `SetDisplayMode` and starts a new one on a new GL context; on the way out of a game the old
  thread's last presents come hundreds of milliseconds apart while the game thread is in the
  exit path (`0x491AA0..`), and the shell is already flipping ~5 000 times a second. The
  publisher kept publishing into a queue nobody drained — and it publishes a lot: a batch every
  5 ms carrying the box bytes of every non-sprite op (~150 KB in game: the minimap copy, the
  bars, the strings), so the 16 MB arena is **half a second** of backlog. Then the overflow
  policy reset and re-seeded into the full arena at every cadence: `arena-full` 24 times in
  120 ms, with 3 643 ops queued. A time rule alone never fired, because the tail *was* creeping.
- **The stale queue was replayed into a fresh context.** The render thread's `glreset` emptied
  the twins and the atlas, then drained ops published before the producer knew: every sprite
  among them, whose bytes had been sent long ago, was `lost` — 705, deterministic, the shell's
  first batches — and each raised `reseed` again.
- **The main offscreen is freed to the heap, not through `SurfaceFree`** (`MEM_Free` at
  `0x491AB8` and at `0x49838C`), and the next `"OFFSCREEN"` may land on the same base (a
  same-base size change) or on another. In the first case the ring still held 1024-wide boxes
  against a 640-wide surface — one `box-outside-surface` overflow per switch; in the second the
  dead entry stayed registered — the census walked it and **crashed** on the first return
  (an access violation at the old base, once the heap had returned the block), and one of the
  24 surface slots leaked per cycle.

### How it works as built

- **The stall guard** (`consumer_stalled()`, game thread, at every publish): the batch is dropped
  — nothing queued, no counter but `stalls=` moves, once per episode — when the tail has not
  moved for 250 ms with work queued, *or* when the backlog is past half the arena or a quarter
  of the ring; publishing resumes, with one reseed (`stall-over`), once the queue is empty or
  the backlog under the low-water marks. A long render hitch (the terrain atlas at a map's first
  frame, a screenshot) counts as a stall and costs one reseed, which is the cheap side.
- **Skip to reset** (render thread): after `tagpu_gui_glreset` the drain takes the arena bytes of
  every op and applies none until the producer's `RESET` arrives (`skipped=`).
- **The offscreen's identity**: a surface created with the tag `"OFFSCREEN"` (`0x5091D4`, the
  five sites) retires every other entry so tagged — the engine has one at a time — and a
  same-base size change forgets the ring's boxes on that base. The census also probes a
  surface's first and last row before diffing it and drops one that is unmapped.
- **Every reset is logged with its reason** under `log` (`gui: reset #n: <why> (queued= arena=
  surfaces=)`), and the heartbeat gained `stalls= skipped= palchg= paldiff=n@i palsrc=`.
- **The palette.** Every palette the engine sets goes through `0x4BA200`, which keeps the
  entries in the graphics globals and hands DirectDraw `min(255, entry × gamma)` with the gamma
  from the Gamma option (`SetGamma 0x4BA590`, `0.5 + Gamma/24`, 1.0 at the default 12) — and
  never scales `main+0x143A7`. The engine's own pixels beneath the twin are shown by cnc-ddraw
  through the palette its `SetEntries` received, so that is what the twin resolves through now:
  the primary's palette object in this DLL, read under the fork's lock, the engine's table the
  fallback until a primary exists. `+gamma N` in chat (`0x417290`) sets the factor to N/10 from
  any skirmish, which is how the walk proves it. `guipal`, loaded at game entry, turned out to
  be the GUI's *logical* palette (256 entries nearest-matched into `main+0xDCB`), and the
  "menu fades" of the corpus are the campaign glamour screen's palette stepper, which nothing a
  skirmish does reaches — whatever runs it, each step is a `SetEntries` the twin's palette
  texture is re-uploaded from at the next present.
- **The loading screen is presented once.** Game entry paints `loadgame2bg` (`0x4288D0`, not the
  palette init the resolution page called it) and flips; nothing presents again until the map
  is loaded and the mode switches — a shot asked for during the load blocks until the game's
  first frame. So the walk arms both capture triggers *before* clicking `Start`, and the fork's
  surface-shot trigger is now polled every present like the GL one (it was every eighth, and
  that frame is one). The world is then waited for after the mode switch, because the overlay's
  `units:` line keeps reporting the dead game's array from the shell.

### Measured

`tools/uiwalk.py --layer --cycles 3` (`strict`, every world pass armed), a lone ARM instance at
1024×768: the shell inventory, 32 in-game stops, then three game→shell→game cycles — the exit
dialogs, the whole shell inventory again, the loading screen, the fixture re-applied, the side's
screens, `+gamma`. **120 stops, every one clean**: 0 differing outside the viewport, 0 inside it
(every non-key engine pixel matched), 0 holes — except the shell's `MAINMENU` and its two
`-back` returns, whose 179–192 differing pixels are the sparkle animation between the two shots,
as in G15b. Across the run: **`overflows` 0, `lost` 0, `resets` 10** (2 per launch + 1 per the 8
context switches three cycles make), each logged with its reason — every one after the first two
is `stall-over`; **`stalls` 9**, one per switch. Twins and the atlas returned to the same figures
each cycle (the atlas is emptied by the context change regardless). The `stall-over` guard cost
one reseed per switch where G15c's DLL cost **38 `arena-full` overflows, 39 resets and 705 lost
sprites** — the storm the first cycle exposed, gone.

| what | 1024×768, 3 cycles |
|---|---|
| stops clean (0 out / 0 in / 0 holes) | **120 of 120** (bar the sparkle skew on 3 shell stops) |
| the loading screen, each cycle | 0 differing, 0 holes, engine surface = GL frame at 640×480 |
| `+gamma 15` (in game) | 235 palette entries differ from `main+0x143A7` (first at index 1), and the twin still matches the engine's frame **0 / 0**; `+gamma 10` puts it back to 0 |
| `overflows` / `lost` / `resets` / `stalls` | 0 / 0 / 10 (all but 2 `stall-over`) / 9 |

**The census, over one full cycle** (`--game-only --screens-only --cycles 1`, no layer):
**9 498 973 pixels changed on the presented surface across 38 stops, 0 unexplained** — the
loading screen among them (1 638 444 changed, 0 unexplained, over its 9 flips before the mode
switch). The residuals on *other* surfaces were the known ones (the startup `OFFSCREEN` written
whole, the `SAVE UNDER` snapshots) plus, once, the line the fix added: `surface … is unmapped —
freed behind the observer, dropped`, where the census retired the game's freed offscreen instead
of faulting on it.

**The palette proof.** `paldiff=n@i` in the heartbeat is the number of entries where the palette
the engine presents with differs from `main+0x143A7`, and the first: **0 in game at the default
Gamma, at most 1 (index 9) in the shell, 235 (from index 1) after `+gamma 15`** — and at every
one the twin's own frame matched the engine's exactly, because both go through the presented
palette. A pass reading `+0x143A7` would be wrong by those 235 entries; that is the open item
below.

**At 1920×1080, the same walk (game inventory + 3 cycles), a lone instance: 88 of 88 stops
clean**, loading screen exact each cycle, `+gamma 15` = 235@1 each cycle, `overflows` 0,
`lost` 0, `resets` 8, `stalls` 7. Two caveats, both run down:

- **The holes are a concurrent-load artifact.** A first 1080p pass run *beside two other
  instances* painted magenta at two stops — `YESORNO#1` (39 800 px) and `ARMOPT#3` (9 734 px),
  the just-opened dialog — where a stall was in flight and the `glshot` caught the twin a frame
  behind its reseed; `selfdiff` 0, so the engine's own frame had not moved. Alone, every one of
  those stops is 0/0/0 (above). And it is a `strict`-only artifact: with the fallback on (a
  player), a twin a frame behind shows the engine's own pixels, never magenta.
- **One isolated pass lost its instance at the *third* in-process map load** (no `ErrorLog`, so
  not a TA fault — the process exited). It did not reproduce: the 1024×768 run does three full
  cycles, the loaded 1080p run did three, and this lone 1080p run on the landing DLL did three,
  none dying. While chasing it a real bug was found and fixed — `surf_drop_offscreens` took a
  `SURF*` that `surf_drop`'s swap-remove can move, so it now keys on the base — which is the DLL
  these numbers are from; whether that was the cause is unproven, since the death never recurred
  to test against.

### Not closed here

- **The world passes read `main+0x143A7`** and are wrong by the gamma factor at any Gamma but
  12 (or after `+gamma`): the UI twin is right and the terrain, units, features and effects
  beneath it are not. The fix is the same source, in `tagpu_native.c`'s palette upload; it is
  not a UI change and was not made here.
- **The fork keeps the game-sized window from the second return to the shell** (§7): at
  1920×1080 the first return gives a 640×480 window and the next two a 1912×1040 client with the
  shell scaled into it; those stops are reported "not 1:1" and not measured. The layer draws
  through the same viewport transform as the engine's frame, so what the player sees is
  consistent — but blurry, and it is cnc-ddraw's window policy that changed, not ours.
- The shell's `SOUND` screen is still not reached (the button is not named `SOUND`/`SOUNDS`);
  the walk reports it and moves on, as it did in G15b.
- The `LIGHTBAR` wipe and the glamour fade are reached by the campaign flow only; neither is
  captured frame by frame. The fade's every step is a `SetEntries` the layer follows by
  construction, and its code is now read (engine map).
- The publisher's 5 ms cadence, three times the present rate in game, and the per-flip clear:
  §7.

---

## 13. Phase 2 — the UI scaled  [DECIDED 2026-09-08]

*The second interview, held 2026-09-08 against the built phase 1. It settles the eight questions
§1 deferred and supersedes five decisions of §3 taken when phase 2 was still a sketch. Phase 1 is
untouched by all of it: every decision below is either additive or applies only at `k ≠ 1`.*

### 13.1 Mechanism: the engine runs at a logical resolution, everything of ours at the device's

**M1** [DECIDED]. The engine is given `window / k` as its screen; our world pass and our UI both
render at the device resolution; `k` is free, not restricted to integers.

The alternatives were weighed and rejected. Patching the layout constants at
`0x4981C9..0x498237` so the engine believes the panel is `128 × k` wide leaves every gadget's hit
rect at 1× — the engine's own UI pixels then land in the *wrong place*, not merely soft, and
`strict` stops meaning anything. Bending the pointer per screen region instead leaves us owning
hit-testing for a tree the engine rewrites at load time.

**M1 costs nothing in input, because the fork already does it** [VERIFIED 2026-09-08, `wndproc.c`
`912`, `dd.c:1000`]: every mouse message is already scaled by
`mouse.unscale_x = game_width / render.viewport.width` and clipped to the letterboxed viewport.
With the engine at 1280×720 in a 1920×1080 window the pointer is already divided by 1.5 before
the engine sees it, so gadget rects, the minimap click rect at `main+0x142BB`, the input firewall
and `vpwide`'s zoom bending all stay in one consistent logical space and **none of them are
touched**. The world half is the existing 2× supersample generalised from 2 to `k` with the
resolve-down dropped [SOURCE `tagpu_native.c:2411`, `tagpu_ss.off`].

**What it relaxes.** [native-res design](native-res-design.html) §0 forbids "an invented internal
resolution … never a mixed-resolution hack". That constraint was written at Phase D start, when
the engine's frame *was* the picture and the question was whether to draw units at a resolution
the player had not asked for. It no longer describes this situation: every visible pixel is ours
at the device resolution, and the engine's frame is the oracle and the fallback, never the
picture. The relaxation is to the engine's internal grid alone, and it is recorded there too.

### 13.2 The 1× mirror stays; a device-res sharp layer is added beside it

**Supersedes §3.1 and §6's "the UI twin's size is `surface × k`"** [DECIDED]. The index twin
stays exactly what phase 1 built — 1:1 with the engine's surface, same ops, same seeds, same
census. Beside it sits an additive **sharp layer**: one device-res RGBA texture carrying only
what we can genuinely draw better at scale — restored art, string ops, our cursor, our minimap.

Composite order, top down: **the sharp layer where it has coverage → the 1× mirror scaled by k →
the engine's surface**.

> **Amended 2026-09-08, before any code: the sharp layer is not one texture.** Restored art is
> drawn into surfaces that have no screen position at draw time — the side panel is painted into
> `panel+0xBC` once and *copied* to the frame when dirty, and we replay that as a twin→twin
> `PK_COPY`. Colour that lived only in a screen-space layer would have nowhere to be written and
> nothing to travel through. So the sharp layer is **two things**: a **colour channel on every
> twin** (§3.4's per-surface RGBA twin, which was right about this, with copies carrying both
> channels), and a **screen-space device-res layer** for what is drawn at present time from live
> state — the cursor (§13.5), and string ops if they are rendered late rather than into their
> surface's colour twin. The composite order above is unchanged, and so is every other property
> in this section. Three properties follow, and they are the reason for the choice:

- **Phase 1's verification survives as phase 2's regression.** The mirror is still bit-comparable
  against the engine's own surface at 1:1, so the `strict` walk's 120 stops and the parity md5 go
  on meaning what they meant, instead of being replaced by a weaker regime invented for scale.
- **A gap in the sharp layer is soft, never a hole** — it falls through to the mirror, which is
  exact.
- The two hard problems decouple: getting text right no longer risks the panel's exactness.

A `surface × k` twin was the alternative (sharpest, one texture, no new compositing rule) and it
loses all three: every captured op becomes a nearest blow-up, and no diff against the engine is
defined at `k ≠ 1`.

### 13.3 The mirror scales by sharp bilinear, and must be identity at k = 1

**Supersedes §3.6's "in phase 2 captured pixels scale by nearest"** [DECIDED]. Nearest at a
fractional `k` staggers every 1-px feature — a bevel alternates one and two device pixels along
its length, and the eye reads the pattern. The filter is therefore a 4-tap with a ramp one device
pixel wide: flat inside a texel, steep across its boundary.

It has to run **after** the palette lookup: the twin's R channel is an index and interpolating
indices is meaningless. Coverage (G) is thresholded exactly as the fog grid's corner bits already
are [SOURCE `tagpu_glsl.h`, "bilinear coverage over the four corner bits, thresholded at 0.5"].

**The gate exit is that at `k = 1` the ramp is exactly one source texel wide, so every output
pixel samples a texel centre and the frame is bit-identical to today's `texelFetch`** — the
parity md5 must not move. That property is what makes 13.2's claim true rather than hoped.

Second-order: because we composite all three layers ourselves at device resolution, the fork's
own `GL_NEAREST` frame upscale [SOURCE `render_ogl.c:764`] is bypassed at `k ≠ 1`. There is only
ever one scaler in the picture, and it is ours.

### 13.4 Text becomes a string op — TA's own glyphs, drawn by us

**R1** [DECIDED], the sixth op kind §3.6 anticipated. `PK_STRING` carries the string, the font
object, the fg/bg colours and the destination instead of a box of captured bytes; the render
thread stamps glyphs from the coverage atlas `tagpu_text.c` already builds from TA's own 1-bit
font — the module G13p proved on the world's `ShowRanges` labels and group digits.

What it requires: the observer must read the string, `GFX_FONT 0x204` and `GFX_FG 0x208` (written
by `SetFont 0x4C1420` / `SetTextColors 0x4C13A0`) rather than only measuring; the string bytes
must be copied into the arena **on the game thread**, for the reason sprite pixels are; and the
engine's measure loop must be reproduced exactly, because `0x4CCF60` does no clipping at all —
`tagpu_text.c` already replicates it character for character.

What it does **not** do is make text sharper. A 1× glyph carries 1× information however it is
drawn; the gains are that a glyph's edge no longer drags in the panel art it was blitted onto,
that text composites correctly over restored Classic++ art, and that the arena carries ~40 bytes
where it carried ~968. A synthesised higher-resolution glyph (an SDF fit to the same letterforms)
would be genuinely crisper and is **not** taken here: at TA's 8–10 px cap height it rounds
corners, and it is the only rung that invents letterform information. It waits on a look at R1 at
`k = 1.5`, not on an argument.

**The exit that keeps this honest: at `k = 1` the string op must reproduce the engine's own
glyphs bit for bit.** If our stamp and the engine's blit disagree by a pixel, the walk says so.

### 13.5 The cursor is ours, at a fixed 1× device size

**Supersedes §3.7's "suppressing the four engine blits"** [DECIDED]. It is drawn in the sharp
layer from live state on the render thread at present time — no queue op is needed, because
everything required is already read every frame: `cursor_rect()` takes the position from the
graphics globals and the size from the sprite record at `+0x1B2`, whose layout the engine map
already carries from the disassembly — **size at `+0`/`+2` zero-extended, hotspot at `+4`/`+6`
sign-extended, so a hotspot may be negative** — which is the same shape as a GAF frame header
(`GF_W/GF_H/GF_HX/GF_HY` in `tagpu_gui_leaves.h`). `+0x1B6`/`+0x1BA` is the position it was last
*drawn* at, i.e. already hotspot-corrected.

Suppression is unnecessary: the engine's blits may keep running into a frame nobody sees, and the
layer's cursor branch simply changes from *discard* (let the engine's show through, phase 1) to
*mask the fallback in that rect*. Drawing from the true device pointer also puts it ahead of the
engine's last-drawn position, which is what G13m spent a gate achieving.

**Its size is 1× device pixels at every `k`** — the convention every scaled desktop UI follows,
and it is always crisp. The accepted cost is a small pointer against a 3× UI at 4K, where TA's
cursors carry gameplay meaning (build, reclaim, attack); a `cursorscale=` knob defaulting to 1 is
the escape, not a change of default.

### 13.6 The minimap is regenerated — rendering ours, visibility the engine's

[DECIDED]. Two halves, and the split between them is the point.

**Ours:** the base picture, the fog, the view box. The base comes from the TNT's own
`TED_GENERATED_PIC`, which is **252×252 (or 252×256)** [SOURCE [file formats](file-formats.html),
`PTRminimap`] while `BuildMinimapSurface 0x466780` fits it into a 126-px box — the engine throws
half of what it has away, so drawing it at its native size is a free 2× with no new data path and
no colour drift. The frame is snapshotted at map load, because the loader frees it at `0x483DF3`
/ `0x483E0B`. Fog comes
from the corner-mask grid we already hold as an RG8 texture for every world pass. The view box is
already ours at zoom [SOURCE `tagpu_zoom.c:557`].

**The engine's:** the dots. They are already sprite ops on `main+0x142DB` and their positions
derive from map coordinates, so replaying them at ×2 into our 252-px target is exact and free.
**This is a rule, not a preference: which units appear on the minimap is fog- and LOS-dependent
sim logic.** Re-deriving it does not fail by rendering badly, it fails by showing enemy positions
the player is not entitled to — a cheat, and in multiplayer *the* cheat. Base, fog and view box
are presentation of data the player already has; dots are not.

Rendering the base from our restored terrain atlas instead was weighed: unlimited at any `k` and
Classic++-restored, but the TNT's picture is a *conversion* of the map rather than the terrain, so
our minimap would no longer match the engine's colours and the oracle would be gone on exactly the
surface where we see least (13.8). It stays what §1 and §3.8 called it — a candidate — and S1
builds everything it would reuse.

### 13.7 The window never resizes, and k chooses itself

**The window** [DECIDED]. The player picks a size once; entering a game and returning to the
shell never changes it. The shell is atom-locked at 640×480 whatever we do, so it is letterboxed
into whatever the window is at `k = min(winW/640, winH/480)`. This makes G15d's open oddity —
"the first return gives a 640×480 window and the next two a 1912×1040 client with the shell
scaled into it" (§12) — **the wanted behaviour, universally**: it is the *first* return that is
wrong, and the fix is to stop the engine's `SetWindowPos(640, 480)` at `0x491AFB` from shrinking
a window the player chose.

**k** [DECIDED]: automatic, `clamp(winW / 1280, 1, 3)`, with a cfg key for the stubborn. No
settings surface, so nothing waits on the Options menu of [renderers](renderers.html) §2.10.

**A consequence that is not cosmetic and must be checked, not assumed:** because the logical
resolution is `window / k`, it lands near 1280×720 on every monitor, so **every player sees the
same amount of world**. Today a 4K player sees far more map than one at 640×480. Equalising that
is defensible and probably desirable, but it changes what a player sees in a competitive game and
belongs under the standing multiplayer rule rather than under a rendering gate. The 1280 baseline
is itself a guess that wants a look on three monitors.

### 13.8 What phase 2 leaves alone, and why

- **Nothing is ever suppressed. It cannot be**: a pixel op reads the engine's finished surface at
  publish time [SOURCE `tagpu_gui_hook.c:318`], so the mirror is built out of the engine's own
  pixels and the engine must keep drawing the whole UI forever. Suppression could only ever apply
  to ops we replace by identity, and buys nothing — the engine's draw is already free at 60 fps.
- **The fallback survives scale.** At `k ≠ 1` a miss is the engine's own pixel through the same
  filter: soft, and exactly in place. `strict` remains the harness's mode only.
- **The blit variants stay captured pixels by design.** The shaded and sub-frame GAF blits, the
  descriptor blit, the textured triangles and GAF frames past `TAGPU_GAF_DECMAX` never need real
  replay paths: the mirror carries them exactly and the sharp layer is additive. That is a
  property of 13.2, not a debt. The only thing that would change it is Classic++ wanting their
  art restored, which is G15e's question.
- **Classic++ needs no new answer.** Everything in the UI is 1× information — the restorer removes
  dithering, it does not invent resolution — so at any `k` the UI reads as one coherent surface
  cleanly scaled rather than a mix of crisp and soft. The deliberate exception is 13.5's cursor.
  True 2× *art* would be a super-resolution model, which is a different model.

### 13.9 The gates

| gate | builds | exit (measured) | kill / pivot |
|---|---|---|---|
| **G17a** the seam | the sharp-bilinear filter, the sharp layer's texture and the composite order; `k` plumbed everywhere but forced to 1 | the parity md5 equals main's and the 120-stop `strict` walk is unchanged, with the filter in the path | the filter is not bit-identical at `k = 1` → stop; nothing downstream is safe until it is |
| **G17b** `k ≠ 1` live | automatic `k`, the logical mode, the world pass at device resolution, the window policy | a walk at `k = 1.5` and `k = 2`: every stop renders; **clicks land on the right gadget at every stop** (a click test, not a pixel test); no resize across three entry/exit cycles; the 1× mirror still diffs exact at `k = 1` in the same run | hit-testing drifts → M1 is wrong and the phase stops, since 13.1 is what makes the rest free |
| **G17c** the cursor | ours in the sharp layer from live state, the fallback masked in its rect, `cursorscale=` | crisp at `k = 1.5` and 3, under the true pointer; G13m's motion-frame measure re-run | — |
| **G17d** the string op | `PK_STRING`, the observer's string/font/colour capture, the atlas draw | text clean at `k ≠ 1` **and bit-identical to the engine's glyphs at `k = 1`**; arena bytes per batch down | our stamp and the engine's blit disagree → the measure loop is wrong; fix it rather than accept a near miss |
| **G17e** the minimap | the 252-px base snapshotted at load, our fog from the corner-mask grid, the engine's dots replayed ×2, our view box | sharp at `k`; dot positions within a pixel of the engine's; **no unit visible that the engine does not show** | the fog rules disagree (13.10) → keep the engine's fog as a pixel op and ship the base alone |

Reviews per the house rule: G17a and G17b at `high` (a new byte patch at `0x491AFB`, and the
composite seam), G17c/d/e at `medium` unless they add a patch.

### 13.10 Open after this interview

- **The minimap's unobserved writers (§7).** `0x4C0070` and `0x4BEE60` are correct only because
  the base copy ahead of them degrades to a pixel op read at publish time; seeding copy sources
  would break them. **G17e must account for the arcs deliberately**, since a base we regenerate
  ourselves no longer carries them for free.
- Whether the engine's minimap fog (`0x466C20`, direct byte writes nothing observes) uses the same
  rule as the corner-mask grid our passes sample. G17e claims parity and has not earned it yet.
- `clamp(winW / 1280, 1, 3)`'s baseline: a guess, wanting a look on three monitors.
- The multiplayer consequence of a constant logical field of view (13.7).
- An SDF or supersampled glyph atlas (13.4), deferred behind a look at R1 at `k = 1.5`.

---

## 14. G15e — Classic++ UI: colour per surface  [MEASURED 2026-09-08]

**Built.** `tagpu_gui_surf.c` gains a colour twin per surface and the palette rule; the two
shared pieces are `MAX_JOBS` 4 → 6 in `tagpu_restoreglsl.c` (priorities 0–3 are terrain,
features, effects and 3DO units, so the UI's 4 had no slot) and `restoreMinEdge` on
`TAGPU_GAFATLAS`, honoured in `restore_enqueue`, which every other atlas leaves at 0. No engine
patch, and **no new engine address** — the whole landing is GL-side over machinery G14e and
G15b already built.

### How it works as built

- **Colour is per surface, `COLOR_ATTACHMENT1` of the twin's own FBO.** The sprite and copy
  programs became MRT: one draw writes `oIdx` (index, coverage) and `oCol` (restored colour,
  alpha 1 where there is one) together, so the two can never disagree about a texel. The colour
  texture is made lazily by the first op that has colour to put in it, and an FBO that comes back
  incomplete is torn back down to one attachment rather than left to swallow the index draws too.
- **§13.2's amendment is why, and the build confirms it.** A copy carries both channels, because
  the panel is painted into `panel+0xBC` once and blitted to the frame later — restored art
  reaches the screen through `PK_COPY` or not at all. A copy from a source with *no* colour twin
  writes zero, which invalidates the destination over the box: a copy from indexed art means
  indexed art. Seeds and pixel ops carry indices only and drop the colour of their box.
- **The layer chooses per texel** — restored where alpha is 1, the live palette everywhere else —
  so a surface only half restored is never half *wrong*.
- **The palette-validity rule (§3.4), in full.** `tagpu_rglsl_job_new` snapshots the palette into
  a texture of its own, so restored colour is a function of the palette that was live when the
  job was made. Every frame that is `memcmp`'d against the palette the frame is **presented**
  with (G15d's, not `main+0x143A7`). While they differ the colour twins are ignored; once the
  palette has held still for 30 frames the job is freed and rebuilt against the new one and every
  colour twin is invalidated, so the art returns restored as the engine redraws it.
- **The floor and the lever.** `restoreMinEdge = 12` keeps the restore queue off frames below the
  model's receptive field — G15-0's verdict, "nothing under 12×12". `norestore` in
  `tagpu_gui.on` is the A/B: the layer without Classic++ art, so the UI half can be toggled live
  without touching the world's restorer.
- **The UI steps the restorer when nothing else does** [the landing review found this]. The only
  other caller of `tagpu_rglsl_step` is the native pass, and it returns early when there is no
  unit array — **in the shell, and in game with the world passes disarmed**. The UI atlas is the
  one atlas that exists there, so without this its queue is never drained: every sprite would
  read alpha 0 from an unpainted twin and the UI would stay indexed for ever, silently and with
  nothing in the log to say why. `tagpu_rglsl_calls()` (a call count, new) compared across
  presents says whether the native pass stepped this frame; when it did, the UI does nothing, so
  the 12 ms budget is sliced once either way.

### Measured

Two Continents, `scenarios/tascene-parity.json`, 1024×768, every pass armed plus `classicpp.on`,
a lone instance:

| what | reading |
|---|---|
| the job exists (the pool raise) | `restoreglsl: gui: lazy restore armed (2048x2048 twin …)` |
| the arm | `gui: Classic++ UI armed — … priority 4, nothing under 12x12` |
| heartbeat | `cpp=1 col=13/3 colvalid=1 rgb=40` |
| queue health | `overflows=0 lost=0 resets=2 stalls=1` — G15b/G15d's norms, unmoved |
| **frame rate with it on** | **`fps=60.0`** |
| **restored vs `norestore`, same DLL, same frame** | **37 435 of the 45 056 px of the in-game menu's panel rect differ; 40 079 whole frame** |
| the palette rule, via `+gamma 15` | `paldiff=235@1` — G15d's own figure — colour dropped, **`rearms=1`**, the colour twins invalidated, `colvalid=1` again against the new palette |
| **the shell** (640×480 `MAINMENU`, after the review's step fix) | **7 963 px differ** against `norestore` — before the fix the shell could not restore at all, structurally |
| in game, re-measured after the five fixes | 40 063 px / 37 415 in the panel rect, `fps=60.0` — unmoved |

**The first A/B differed by 0 pixels, and that is the finding.** In a running game the panel is
*seeded* at the mode switch, and colour reaches a twin only through a sprite op — so the atlas
then holds nothing but the small HUD icons: **25 entries, none larger than 10×12**, every one of
them under the floor by design (`tagpu_restore_gui.idx`). Opening `ARMOPT` draws real UI art as
sprite ops, the atlas goes to 44 entries, and the panel restores. So **on entering a game the
panel is indexed until something repaints it** — a mode switch, a build page, the menu.

### After the review

One Opus reviewer at `medium` on the landing diff, five findings, **all five verified against the
code and all five acted on**:

- **The restorer never stepped where the native pass returns early** — the shell, and in game with
  the world passes off. This was the real one: the shell's UI could not restore at all, and the
  module's own comment offered the shell as the *good* case. Fixed above; the shell measurement in
  the table is the proof, and it is a number that did not exist before the fix.
- **`upload_palette()` ran twice per frame** — once in `tagpu_gui_present` before `restore_step`
  decided `s_colValid`, and again inside `draw_layer` after a drain thousands of ops long. If the
  game thread set a new palette in between, the frame drew restored texels resolved through the
  palette their restore snapshotted beside indexed texels resolved through a newer one — exactly
  the "wrong art" §3.4 exists to prevent. The second call is gone; one upload per frame, and it is
  the one `s_colValid` was decided against.
- **The re-arm line printed `s_ntwins`**, the total twin count, where it claimed to report the
  colour twins invalidated — and this page quoted that number as a measurement. It counts them now,
  and says "N of M twins had colour".
- **`unbind_all` left texture unit 3 bound**, against its own stated invariant, once the layer
  started binding the colour twin there. No consumer reads unit 3 unbound today; fixed as hygiene.
- **The heartbeat buffer was still too small**: 194 bytes of literal plus 27 conversions is ~491
  worst case against 420, and `_snprintf` does not terminate what it truncates. 640, and
  terminated explicitly.

The reviewer separately verified clean, and these are worth recording because they are the
properties the landing rests on: the indexed path is untouched with `classicpp` off or `norestore`
set; `MAX_JOBS` 4 → 6 has no bitmask or baked bound behind it; `restoreMinEdge`'s 0 default is a
true no-op and `restore_enqueue` is the only enqueue path; the copy-from-untwinned-source
invalidation works in both the C and the GLSL; and `clear_dest` really does clear the atlas twin to
alpha 0, so a sub-12-px frame falls back to indexed rather than to garbage.

### Not closed here

- **The gate's Q2 diff against G15-0's offline output is not run.** The sheets are judged
  (2026-09-08, every class passes) and the fps and fade halves are measured; the numeric bar
  against the offline restore of the same frames is owed.
- **Seeded art stays indexed** until redrawn (above). Restoring a seed directly — the surface is
  an indexed image and the restorer restores indexed images — is the obvious candidate and is
  not taken here.
- **The `uirestore` name globs and the sequence-name registry are not built.** Cursors are
  excluded *structurally* — the cursor's blits are excluded at the source, so its frames never
  enter the UI atlas at all — and the 12-px floor covers the rest of the ruled default; only
  `pathicon` by name is unreached. `tagpu_gui_art.c` (§4) still does not exist.
- **The phase-1 `strict` walk is not a valid regression while this is on.** It compares our frame
  against the engine's **indexed** surface, so every restored pixel reads as a difference. Run it
  with `classicpp` off; the restored half needs a check of its own, which is the Q2 diff above.
- The heartbeat's line outgrew its 260-byte buffer when these counters were added and silently
  truncated `fps=`; it is 420 now. A counter added to that line without widening it again will
  do the same thing.
