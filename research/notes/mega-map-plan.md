# Mega map and strategic icons — plan

*A plan note: the design the owner chose on 2026-11-02, and the landings that build it. Facts carry their source; the scout's full inventory, with every
citation, is archived at `.data/mega-map/facts-2026-11-02.md` in the main checkout.*

## What it is

**One code path, two ways in.** The world view zooms out until the whole map fits the screen, and
units turn into strategic icons as they get small, by BAR's rule. The *mega map* is not a second
renderer: it is a jump to the whole-map level, TADR's gesture on our camera.

- **Wheel** zooms as today, but no longer stops at 0.25×: the floor is the level at which the
  current map fits the world viewport.
- **Ctrl+wheel back** engages the mega map: a tween to the whole-map level about the view centre,
  remembering the level it left.
- **Wheel forward while engaged** leaves it: a tween back to the remembered level, centred on the
  world point under the pointer (TADR's `WheelMoveMegaMap`, with the player's own zoom kept).
- **Legacy controls** (`impure.cfg`, off by default) swap the two: plain wheel is the mega map,
  Ctrl+wheel is the zoom — TADR's binding, for players who learned it.
- **Full command** at every level: selection, band box and orders work through the same
  `vpwide` path that already serves zoom < 1.
- **Icons**: generated team-coloured shapes by class by default; a mod's own TADR icon set
  (`Icon/iconcfg.ini`) when its install ships one.

**Why not TADR's design.** TADR's megamap is a separate 2D picture of the map with its own input
translation for every order. Ours already draws the world, and selects and commands in it, below
1×; the only things missing are the levels below 0.25 and the icons. The TADR-port notes listed the
megamap as a deliberate exclusion (`tadr-port/quality-of-life.md`, `tadr-merge-exploration.md`);
this plan replaces that line, and the landing that ships it corrects those notes.

## What BAR does — the rule we copy

Sources: RecoilEngine `9e0c169`, Beyond-All-Reason `1d90016` (clones in the session scratchpad;
re-clone to verify).

- **BAR forces screen icons** (`UnitIconsAsUI=1`, `luaintro/springconfig.lua:126`). An icon is a
  screen-space square of constant size, `max(vw, vh) × 0.005 × UnitIconScaleUI` (BAR 1.05) times a
  per-type `mult = 0.75 × size + 0.25` (`UnitDrawerData.cpp:217, 375-382`).
- **One camera distance per frame**, `iconZoomDist`, from the camera to the ground along the view
  ray (`UnitDrawerData.cpp:222-230`).
- **A unit in LOS becomes an icon** when `iconZoomDist / mult > UnitIconFadeStart` (3000)
  **and** its model's screen radius is under 0.9× the icon's half-size (`:401`). The model is not
  drawn while it is an icon. Bigger icon types switch later.
- **Radar-only enemies** are always icons: the generic radar dot until identified, the real icon
  once seen with continuous radar contact (`GetUnitIconIndex`, `:300-322`). Buildings out of radar
  keep a ghost icon at 0.75 brightness.
- **Colour**: the icon art × team colour; **selected units are white** (`UnitDrawer.cpp:921-923`).
  No hover indication.
- **Order**: commanders over air over ground over structures, then far before near
  (`UnitDrawer.cpp:349-368`). Over the world, under all UI.
- **The fade.** The engine has a fade-in, an alpha ramp from 64 to 255 between
  `UnitIconFadeVanish` and `UnitIconFadeStart` (`UnitDrawer.cpp:833-840`), drawn over the model
  until the model switches off. **BAR disables it**: it sets Vanish = Start on every launch
  (`springconfig.lua:118`), so BAR as shipped is a hard switch. The model itself never fades.
  **Decided (owner, 2026-11-02): the engine's ramp.** The icon fades in from alpha 64 to 255
  across a band before the model switches off; the band is an `impure.cfg` key, and a zero band is
  BAR's hard switch.
- **The far zoom is per map** in BAR too: `1.333 × max(mapx, mapy) × 8` elmos
  (`SpringController.cpp:49`).

**Translating the distance.** TA has no camera distance; its measure is the level `z`, and `1/z`
is world px per screen px. BAR's 3000-elmo threshold becomes a level, calibrated in the lab
(`tools/tascene-view.html`) against BAR's screen at the same visible span. [INFERRED: the order of
magnitude is z ≈ 0.4 at 1080p; the landing measures it.]

## What the floor holds today

`TAGPU_ZOOM_MIN 0.25` (`inc/tagpu_zoom.h:12`). The fit level of the largest stock map, Seven
Islands at 20480² px, is about 0.034 at 1024×768, 0.050 at 1920×1080 and 0.102 at 3840×2160.

- **Six fixed `z > 0.05` guards** fall back to 1× below 0.05: the gather span
  (`tagpu_zoom.c:540`), the vpwide addressable rect (`tagpu_vpwide.c:587`), the command record
  (`tagpu_packet.c:830`), the minimap box (`tagpu_zoom.c:994`), the ScrollSpeed scaling
  (`tagpu_zoom.c:729`) and the sweep (`tagpu_sweep.c:212`). Each must follow the floor.
- **vpwide refuses a rect past 32768 px** (`tagpu_vpwide.c:625`); Seven Islands at 4K needs ~36 300.
  Engine-bound mouse coordinates are int16 (`tagpu_zoom.c:1940`), so the rect itself cannot grow
  past that — the cap needs an argument, not a bigger number.
- **The wide fog grid is built every sim tick at the floor's size**, whatever the level in force
  (`tagpu_fogwide.c:620-645`; 145 µs at 0.25). Its cost grows with 1/floor². A floor that follows
  the map keeps small maps cheap; a large map at the whole-map level needs the grid sized to the
  level in force, which touches the fog handshake (`tagpu_zoom.c`, *never clear bit 3 from the
  render thread*).
- **The terrain draw carries nothing per cell** (1b): one instance per cell of the gather's grid,
  derived in the vertex stage from the level's tile map. Its bound is the grid's side,
  `TAGPU_TERR_MAX_SIDE` = 2052 cells, which follows from the gather rect's cap
  `TAGPU_GATHER_SPAN_MAX` (65 536 px a side, `tagpu_terr.h`): Seven Islands whole is 640 × 640
  cells, inside it.
- **ScrollSpeed** saturates at 255 below z = 0.125 (default 32). At the whole-map level there is
  nothing to scroll to; stated, not fixed.
- **The recorder's camera** clamps the level to 0.25..8: the writer (`tagpu_recorder.c:3873`),
  `rec_pov_ok` (`tagpu_recpov.h:85`, which makes an out-of-range sample *malformed*), and Follow
  (`tagpu_persp.c:65`). **Decided (owner, 2026-11-02): widen the format's range** under a new
  format version, so a replay shows the whole-map view the player saw; older recordings still read.
- The file lever and the Follow lever ignore levels below 0.25 (`tagpu_zoom.c:395-403, 486`).

## The facts the icons need

- **Class from the unit def** (`tools/tamem_ghidra.h:893-995`): flags +0x241 (builder 0x40,
  isairbase 0x200, canfly 0x800, canhover 0x1000, floater bit 19), bmcode +0x22F, YardMap +0x14E,
  weapons +0x1EE/+0x1F2/+0x1F6. **Commander**: the engine's own rule compares the unit's name with
  the side's commander name (`engine/units.md:1051`); the FBI `commander` bit (+0x245 0x40000) has
  false positives (ProTA ARMTHOVR), so the engine's rule is the one to use.
- **Categories** (for mod icon sets, which key icons by FBI category tag) are not in the def: each
  category is a bitmask over UnitTypeID in a sorted name map at `0x51E6B4/B8`. **`0x488C50`
  inserts and allocates on a miss** — it must not be called with a mod's tag; a read-only walk of
  the map is needed.
- **Visibility**: unit +0x110 bit 8 radar (sight sets it too), bit 9 sonar, bit 10 jammed; the
  engine's radar list at main+0x14363 (10-byte entries, count +0x1436B), rebuilt every tick by
  `0x466DC0` — TADR's megamap draws from it. Ours today (`tagpu_native.c:2436-2450`) skips cloaked
  enemies, unexplored tiles and grey-fogged foreign units.
- **Selected** +0x110 bit 4; **hovered** main+0x2CBA; **team colour** PlayerInfo+0x96 (the minimap
  dot's own colour comes from `0x4B7F30` — UNKNOWN what it computes).
- **The packet** carries +0x241 per unit but not bmcode, YardMap, weapons, +0x245 or categories:
  the class is computed on the game thread at level load, per type, and the packet carries it.
- **Picking an icon**: BAR widens the unit's pick radius to the icon (`iconRadius`,
  `UnitDrawerData.cpp:391`). The engine picks by model bounds through HotUnits/`GetUnitAtMouse`;
  HotUnits' capacity is UNKNOWN. A 2-px unit under a 16-px icon must be clickable — research item
  for landing 3.

## Mod icon sets (TADR's format)

Every TADR mod sets `MegaMapConfig=.\Icon\iconcfg.ini` (TA Zero: `.\ZIcon\`) in its TADR ini
(`mayhem.ini`, `ProTA.ini`, `TAESC.ini`, `TAZero.ini`, `TA.ini`/`totala.ini`). The format
(`vendor/TADR/src/DDraw/UnitMinimap.cpp:700-985`):

- `[Option]`: palette indices `FillColor` (→ team colour), `TransparentColor`, `SelectedColor`
  (shown only when selected), `HoverColor`; `UseCircleHover`; `UseDefaultIcon`.
- `UseDefaultIcon=TRUE`: fixed file names in the folder — `COMMANDER`, `CONS`, `FACTORY`,
  `BUILDING`, `AIRCRAFTCOMBAT`, `AIRCONS`, `MOBILECOMBAT`, `NONE`, `UNKNOWN`, `NUKEICON` (`.PCX`),
  plus `<side commander>.PCX` per side (ARM, CORE, GOK). Mayhem, TA Zero and Twilight ship this.
- `UseDefaultIcon=FALSE`: `[Icon]` maps an FBI category tag to a PCX; `Unknow`, `Nothing`,
  `NukeIcon` are special keys. ProTA 4.8 ships 25 (KBOT, VEHFAC, SHIPCONS, HOVER, EXP, …).
- Escalation and retail ship none → the generated set.

The icons are read from the player's install at run time and never shipped, so the
redistribution question does not arise. TADR itself is never loaded (the takeover), only its
ini and art read. Which ini a given install's TADR reads is a research item for landing 4.

## Decisions on landing 1 (owner, 2026-11-03)

The landing-1 spec (`.data/mega-map/landing1-spec.md` in the main checkout) put the cost of the
whole-map level on Seven Islands at 4K at about +110 MB of a 32-bit address space. Almost all of it
is per-frame working copies, not information, so the landing is split to remove them first:

- **The fog grid is shipped as its source.** Today each 32-px cell carries two 4-bit corner masks
  in two bytes — an expansion of 2 source bits per map cell (out of LOS in true-LOS mode; never
  explored), sized to a view window that at the floor is twice the map, and uploaded by each of 5
  passes for each of 4 frames in flight: ~33 MB. The source for the whole map is
  `losW × losH × 2` bits — about 100 KB on Seven Islands. Packed on the game thread, held in one
  texture per frame slot that every pass shares, expanded in the shader as `0x4843C0` expands it
  (corner OR, then the four border completions in the engine's order), checked against the
  engine's grid by the existing oracle. A grid that always covers the whole map cannot be outrun
  by a zoom or an eye step, so the spec's fog-level ladder and its in-flight handshake are not
  needed. RLE was considered and not taken: the GPU needs random access, and bit-packing gives the
  same saving without a decode.
- **Terrain instances are built once per level** if their records are static (tile position and
  cell position, and the restorer key, which a job completing does not change: it paints the
  neighbourhood atlas, not the key), replacing four per-frame mapped copies and an 11.6 MB copy a
  frame at the floor.
- **The MP recording version stays** unchanged: only a mixed-build game is affected, and the newest
  build's recording is whole.
- **The engine hit test's int16 narrowing is guarded at its one call** (`0x48CE15` in
  `GetUnitAtMouse`, the only reference to `0x48C6A0`): the engine's test runs only when the unit's
  model is valid, its root-box radius `R` (the engine's own `0x4CB650`) is at most 8191, and
  `|x − eyeX|`, `|z − eyeY|`, `|alt|` each plus `R` fit int16 in 64-bit arithmetic; otherwise "not
  hit". It checks the actual position at call time, so it holds for units the engine does not bound
  — aircraft off the map or above cruise, teleports, carried units, network positions (scout batch
  B, `.data/mega-map/facts-2026-11-03-B.md`). It replaces a level-load bound from the model box,
  which those units defeat (owner, 2026-11-03).
- **Mipmaps get their own landing after the floor**: per-tile mip chains for the terrain atlases and
  features, the level chosen from the supersampled target's pixel footprint, so 1× and above stay
  byte-identical.
- **Stated, not fixed**: ScrollSpeed saturates at 255 below 0.125; 5120×1440 on a 20 480-px map
  stops at ~0.089 (int16 clicks); 640×480 on maps over ~26 000 px stops at 1/64.

- **The spec, v2** (`.data/mega-map/landing1-spec-v2.md`), and the owner's calls on it:
  - the packet's unit and feature tables keep a window that follows the level in force, through
    a ladder of levels and an in-flight ring — the fog no longer needs one, the tables still do;
  - the packet reserves room for a fog source up to 2048 × 2048 cells (5 slots × 1 MiB, reserved,
    committed only as used): the render thread reads no engine memory, so the game thread copies
    the source into the packet, and a map the terrain draws must also have its fog;
  - at 1× a fogged unit or wreck straddling the bottom or right screen edge is now hidden, as the
    engine's rule says; the old grid ended a cell past the view and left it drawn.
- **The tile map and tile set are immutable for a level** (scout, DISASSEMBLED,
  `.data/mega-map/facts-2026-11-03-A.md`): one writer each in LoadMap `0x483610`, one free in the
  teardown's `0x483DD0`. The tile-set pointer can repeat across two loads, so 1b keys its upload
  on a level serial, not on the pointer.

## Landings

1a. **Fog from its source bits**, whole map, one shared texture, expanded in the shader.
   *Built 2026-11-03 on this worktree; the in-game checks V1a-1..6 are open.* Offline: the build's
   checks, `tools/replay/fog_expand_test.py`, `tools/wiki/scenecheck.mjs` (the stage's fog source
   expands to the mockup's grid byte for byte), and the lab's extracted GLSL ES compiled by
   glslang. Two things the spec did not foresee: the completion order cannot matter on a whole-map
   expansion — two completions meet only at a corner entry, which holds one map cell — so the host
   test checks that all 24 orders agree and that dropping or turning round each completion is
   caught, instead of a swapped order failing; and the browser lab and the wiki mockup extract the
   DLL's GLSL, so both now bind an R32UI source (`tools/tascene` declares `usampler2D`'s
   precision). There is no off switch for the fog: a frame without a source fails closed, drawing
   no unit but the watched player's own and no wreck.
1b. **Terrain from per-level data.** *Built 2026-11-03 on its worktree; the in-game checks V1b-1..3
   are open.* No record is stored per cell: the draw is `cols × rows` instances of the unit quad, and
   the vertex stage derives each cell from `gl_InstanceIndex`, the level's tile map (R16_UINT) and
   the neighbourhood keys (R32_UINT), both uploaded once a level as waits, behind a
   TRANSFER → VERTEX_SHADER barrier on the frame that draws ([terrain & depth](terrain-depth.html)
   §7.1). The copy, the atlas, the height grid and the keys are keyed on the packet's `level_gen`,
   not on the tile-set pointer, which the next level's allocation can repeat. Gone: the per-frame
   records, their CPU staging and its reservation (`tagpu_terr_clamp_span`), and the four per-slot
   mapped instance buffers. What the spec did not foresee: the staging's own barrier makes a copy
   visible to the fragment stage only, so the vertex stage needed its own; the restorer's
   on-screen key feed read the records, and now works the keys out on the CPU by the shader's rule;
   resizing the keys image away and back as the keys came and went would have retired an image at
   every restore drop and blanked the terrain while two retires were pending, so the image keeps
   what it holds; and the browser lab and the wiki mockup extract the DLL's terrain shader, so both
   draw from an R16UI tile-map texture too.
1c. **The floor follows the map.** A runtime floor `min(0.25, fit)` with `fit` from the scroll
   extent and the true viewport, bounded by the int16 click rect; the hit-test guard at
   `0x48CE15`; the seven guards (the cursor transform `view()` is the seventh), the vpwide cap and
   the two levers follow it; every pass's fog-width cap goes; the recorder's camera range widened
   under a new format version (27).
   *Verified*: wheel to the whole map on Seven Islands at 1024×768 and 4K; click and band-box
   selection and a move order at that level; `tagpu_fogwide_check.on` `differ=0`; frame and tick
   time at the floor. Sim-adjacent if the fog grid changes → review at `high`.
2. **The mega map binding.** A modifier bit in the notch slot; Ctrl+wheel back engages, wheel
   forward leaves at the pointer at the remembered level; legacy controls as an `impure.cfg` key
   and a menu row. Replay Follow and the file lever keep their priority.
3. **Strategic icons, BAR's rule.** Class per type at level load (game thread); the packet carries
   it; an icon draw in the marker pass (`tagpu_mark_emit_art` already draws one texel per screen
   pixel at a world point); the model suppressed while iconified, by the same predicate the gather
   uses; radar-only enemies as the radar dot from the engine's radar list; selected white; order by
   class then depth; icon pick radius. Generated shapes. Settings: icon scale, icon distance (a
   closed list, as every `impure.cfg` key is).
4. **Mod icon sets.** Find the install's TADR ini, read `iconcfg.ini`, decode the PCX through the
   palette into an atlas (FillColor → team colour, TransparentColor → alpha, SelectedColor only
   when selected); categories through a read-only walk of the category map.
5. **Documentation** rides with each landing; the last one also corrects the TADR-port notes'
   "megamap excluded" lines and adds an in-game wiki entry.

Presentation only: nothing here writes simulation state. The recorder question in landing 1 is
the one place it meets the replay.
