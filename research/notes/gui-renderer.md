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

**Phase 2 — the UI scaled** [DECIDED 2026-09-06, its own interview later]. At 1080p and above the
128-px side panel and the 32-px bars are drawn at 2× and the world viewport shrinks around them.
The engine's layout constants are fixed pixels (`left=128`, `top=32`, `bottom=H−33`, written once
at `0x4981C9..0x498237` — [resolution](resolution.html) §2), so this means running the engine at a
logical resolution and rendering the world at the device's. Phase 1 must not close that door;
§6 lists what it has to leave intact. The shell is the easy half of phase 2: its layout is
640×480 whatever the window is, so "draw the 640×480 layout into a window-sized twin with smooth
filtering" is a late gate, not a new architecture.

**Not this project**: a redesigned UI; hi-res fonts; regenerating the minimap from our terrain
atlas (a candidate, §3.8).

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

**The census** is the same machinery pointed at the whole surface: at each flip, diff the surface
against its previous copy and subtract every recorded op's box. What remains is a writer we have
not bracketed, with its exact pixels, per screen. It is G15a's exit and every later gate's
regression.

### 3.7 The cursor stays the engine's in phase 1
It never enters a twin (§2), so it reaches the frame through the fallback, as today. `strict`
exempts its rect (position `[obj+0x1B6/0x1BA]`, size from the sprite record at `+0x1B2`). Owning
it — drawing the sprite at present time from the true pointer and suppressing the four engine
blits — is the **first gate of phase 2**, where a 1× cursor on a 2× UI forces it.

### 3.8 The minimap is mirrored like everything else
A sprite op if it goes through a GAF blit, a pixel op through the gadget bracket otherwise; dots
and view box are residual pixels. Under Classic++ the picture is restored as one frame if it
arrives with an identity. Regenerating it from our restored terrain atlas is a candidate for
phase 2, where a 252-px picture at 2× may not be enough.

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
  `tagpu_classicpp.cfg` takes name globs; default `all` minus `cursor*` and anything under 12×12
  (below the 25-px receptive field there is nothing to restore). G15-0's sheets set the default
  exclude list.
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
| **G15-0** offline art spike — **run 2026-09-07, §8; verdict pending** | `tools/undither/uiart.py`, seven contact sheets, the consistency table | the owner's per-class verdict; the default `uirestore` exclude list written down | no class passes → Classic++ UI dropped from phase 1, UI-aware fine-tune filed as a candidate; the Classic half unaffected |
| **G15a** census — **done 2026-09-07, §9** | observer detours on every pixel-writing leaf, the flip marker, the whole-surface diff, the allocator/free pair, the walk script (`tools/uiwalk.py`); **no drawing** | writer table in [the engine map](exe-reverse-engineering.html) (leaf, convention, call sites, surfaces written); unexplained pixels under 1 % on every inventory screen or every remaining writer named; the minimap's draw path located; the two stale claims in `ui-markers.md` §4 and `frame-composition.md` §1 corrected | a core screen with a large untraceable writer → that surface is seed-only, the plan proceeds |
| **G15b** twins, in game, Classic — **done 2026-09-07, §10** | the `tagpu_gui_*` module: registry, seed, queue, replay, the three op kinds, the index twin (the colour twin is G15e's), the composite seam, `strict`, trigger, tacli verb, the default arm set, the transient viewport clear | side panel, build pages, top and bottom bars at 1024×768: 0 differing pixels outside the cursor, 0 holes; fps fixtures within half a frame; ring peak and overflows logged; parity md5 unchanged with the trigger absent; 1080p run and recorded | replay cannot hold 60 fps at `200v200` → collapse identical per-frame ops before anything else (**it happened, for a different reason — §10**) |
| **G15c** the rest of the frame | chat, F4 and bottom dialogs, `ARMOPT` over the viewport, HUD text and clock, minimap picture, dots and box, `LIGHTBAR`, the mode-switch panel painter | whole in-game inventory clean under `strict`, ARM and CORE; dialogs over the viewport verified at 0.5× and 2× | — |
| **G15d** the shell | registry reset and re-seed across the 640×480 context switch, the shell inventory, the loading screen, palette behaviour measured (`guipal`, fades) | shell inventory clean under `strict` at 640×480; three entry/exit cycles with twin and atlas counts flat; any fade visually identical to the engine's | — |
| **G15e** Classic++ UI | the UI atlas's restored twin (job priority 4, lazy on first draw), the palette-validity rule, the `uirestore` policy, the twin dump and diff | Q2 bar against G15-0's offline output on every frame the walk draws; sheets judged by the owner; fps unchanged; a fade shows indexed art, never wrong colour | per-class exclusion per G15-0 |

**Landings and reviews** per the house rule: G15b and G15c land separately — G15b is the
composite seam and the module skeleton, which other worktrees will merge under, so it lands
small and early. G15a and G15b review at `high` (new byte patches), G15c and G15d at `high` if
they add patches else `medium`, G15e at `medium` (a new GL object and a shader branch, no
patch), G15-0 skips the review (tools and docs). Each landing's documentation pass adds its
addresses to the engine map and its rows to [GPU status](gpu-status.html) §2, and this page's
gate table gets its status.

**Unlocked, not scheduled here**: the §2.10 Options menu as a surface of ours; phase 2's cursor,
shell scaling and in-game scaling.

---

## 6. What phase 2 must find intact

- The twin is `surface × k` with `k` a parameter; every op is recorded in logical coordinates.
- Sprite ops keep the frame identity, so the colour twin can be filtered when scaled; the UI
  atlas can take the unit atlas's `pad/align/mip` (4, 4, 2) without a model change.
- Pixel ops are indices with coverage, scaled by nearest; a string op is an addition, not a
  rewrite.
- The cursor is outside the twins.
- The module reads no world state, so the logical/device split lands in `tagpu_gui_scale.c` and
  the composite, nowhere else.

---

## 7. Open  [OPEN]

- Whether the shell changes the palette: `guipal` at game entry is verified, fades are a corpus
  gloss (`main+0x3907F..0x3908B` "menu fades") with no disassembly behind it. G15d measures.
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
verdict per class is pending; what follows is the run and a first reading of it.**

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

**A first reading, for the owner to confirm or overrule:**

- **`unitpics` and the panels pass on sight.** The build icons are exactly what the model was
  trained on; the panels lose their grain and keep their geometry.
- **The order buttons (`ATTACK`, `PATROL`, `REPAIR`…) are the one judgement call.** Their labels
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

**Proposed default for `uirestore` pending the verdict**: `all` minus `cursor*`, `pathicon`,
anything under 12×12, and — if the owner rules the label softening out — the order-button
sequences of `commongui.gaf` by name. Nothing in the run argues for dropping the Classic++
half: the kill rule of §3.9 is not triggered.

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
  whole-inventory run on CORE and the dialogs over the viewport at 0.5× and 2× are G15c's.
- A **clear is published per engine flip** the fill sequence advanced on, and the engine
  free-runs its flip — in a batch of *n* flips only the last clear can matter for the final
  state, so *n−1* of them are wasted scissored clears of the viewport. Cheap at 1024×768;
  see the 1080p row above for whether it is cheap there.
- `tagpu_gui_snap.c` (the gadget snapshot behind `tacli ui`) was moved into the family by G15a
  unchanged; `tagpu_gui_art.c` does not exist yet — the atlas lives in `tagpu_gui_surf.c` (§4).
