# Mega map and strategic icons — plan

*A plan note: the design the owner chose on 2026-11-02, and the landings that build it. Facts carry their source; the scout's full inventory, with every
citation, is archived at `.data/mega-map/facts-2026-11-02.md` in the main checkout.*

## What it is

**One code path, two ways in.** The world view zooms out until the whole map fits the screen, and
units turn into strategic icons as they get small, by BAR's rule. The *mega map* is not a second
renderer: it is a jump to the whole-map level, TADR's gesture on our camera.

- **Wheel** zooms as today, but no longer stops at 0.25×: the floor is the level at which the
  current map fits the world viewport.
- **Ctrl+wheel back** engages the mega map: a tween to the whole-map level with the view centre
  moving onto the map's centre, so the whole map is on screen from wherever it was engaged,
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
translation for every order. Ours draws the world, and selects and commands in it, at every level
down to the whole map; what the game shows there is the world itself, with icons for the units. The TADR-port notes
([quality of life](tadr-port/quality-of-life.html), [the merge exploration](tadr-merge-exploration.html))
and the roadmap's TADR survey state the mega map as this design: a repurposed strategic zoom with
TADR's icon sets read from the install, not a port of TADR's view.

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
is world px per screen px. At BAR's 3000 elmos its 45° camera spans 2485 elmos of ground, 4970 TA
px [INFERRED: 1 elmo ≈ 2 TA px], so the level at which a frame `gh` px tall spans the same is
`z_S = gh / 4970`: **0.217 at 1080p**, 0.290 at 1440 (4K at HUD scale 150), 0.435 at 2160 — scaled
by the `icondist` key as a percent of BAR's distance ([strategic icons](modules/icons.html),
*The rule*). An earlier estimate here, z ≈ 0.4 at 1080p, was 2× off; a lab calibration against
BAR's screen has not been made.

## What the floor is

**The floor follows the map** (landing 1c; [camera and zoom](modules/camera-zoom.html) §2.3,
*The floor follows the map*). It is `min(0.25, fit)`: `fit` the level at which the scroll extent
fits the true viewport, searched on Q16 levels and bounded by the one int16 limit a widened view
meets in a click — the widened rect's four edges, through `tagpu_vpwide_rect_at`, which is also
vpwide's refusal. The game thread computes it at every in-play draw and logs it when it moves
(`zoom: floor … bound by fit|lparam|0.25|1/64`); the packet carries it to the render thread. By
the arithmetic, Seven Islands (20 480² px) gives 0.0346 at 1024×768, about 0.05 at 1920×1080 and
0.1030 at 3840×2160, all bound by fit; a map the screen covers at 0.25× keeps 0.25.

- **The seven guards** that fell back to 1× below 0.05 — the gather span, the vpwide addressable
  rect, the command record's `cmd_valid`, the minimap box, the ScrollSpeed scaling, the sweep's
  level and the cursor transform's `view()` — validate a level against `TAGPU_ZOOM_FLOOR_LO`
  (1/64) instead, the sanity bound on every level value. `TAGPU_ZOOM_MIN` is gone, with
  `TAGPU_ZOOM_FLOOR_HI` (0.25) and `TAGPU_ZOOM_FLOOR_LO` in its place.
- **vpwide's bound is the rect's four edges in int16**, not a 32 768-px width: the button
  `lParam` carries the engine point as two int16s and every engine reader of the rect compares in
  32 bits, so a rect up to 65 535 px wide is exact. Seven Islands at 4K needs about 36 000.
- **The engine's unit hit test narrows to int16**, and nothing the engine keeps bounds what it
  narrows (aircraft off the map or climbing, carried units, network positions, Teleport:
  [units](engine/units.html), *What bounds a unit's position*). Its one call, `0x48CE15` in
  `GetUnitAtMouse`, goes through `tagpu_zoom_hit_guard`, which answers "not hit" for a unit whose
  narrowing could wrap ([input](engine/input.html), *Who reads the eye*).
- **The fog** is the whole map's source (1a), so no level outruns it.
- **The tables' reach follows the level** through a ladder (0.25, 0.125, 0.0625, 0.03125, 1/64)
  and an in-flight ring of the render thread's requests (`reach_zoom` on the command record,
  `reach_level` on the packet): at 1× the anchor table and the models' cull cover the window
  they covered at 0.25, and a zoom-out waits at a rung for the game thread (`reachheld=`).
- **The terrain draw carries nothing per cell** (1b): one instance per cell of the gather's grid,
  derived in the vertex stage from the level's tile map. Its bound is the grid's side,
  `TAGPU_TERR_MAX_SIDE` = 2050 cells, which follows from the gather rect's cap
  `TAGPU_GATHER_SPAN_MAX` (65 536 px a side, `tagpu_terr.h`), which `tagpu_zoom_gather_span`
  applies whatever floor its caller holds: Seven Islands whole is 640 × 640 cells, inside it.
- **The scroll step** is fixed by its own landing (`.data/mega-map/scrollspeed-spec.md`). The
  zoom scaled the byte `ScrollSpeed` and stated that it saturated at 255 below 0.125; it stopped
  working earlier, because the poll caps the byte times the elapsed ticks at 0x80 (`0x41CEAA`): at
  z = 0.25 for the default 32. Writing the byte also cost a save guard, and the options screen's
  snapshot and restores fooled its cached base (a doubled value saved, or the player's choice
  overwritten). Now the poll's capped step is divided by the level in its register at `0x41CEAA`
  and the byte is never written, so the screen scrolls at the 1x rate at every level.
- **The recorder's camera** goes down to 1/64 in recording format 27 (`REC_POV_ZOOM_MIN` 1024);
  format 26 recordings still read, with their own 16384 minimum, and Follow draws a recorded level
  below the viewer's floor at the floor. The multiplayer contribution version is unchanged.
- **The file lever** parses `[1/64, 8]` and **the Follow lever** takes any recorded level; both
  are drawn no lower than the floor.

## The facts the icons need

- **Class from the unit def** (`tools/tamem_ghidra.h:893-995`; the bits the class rule reads are
  checked in the FBI reader, [units](engine/units.html), *The other FBI tags a type is classed by*):
  flags +0x241 (builder 0x40, isairbase 0x200, canfly 0x800, hidedamage 0x4000, canhover 0x1000,
  floater bit 19), +0x245 showplayername 0x20000, bmcode +0x22F, YardMap +0x14E, weapons
  +0x1EE/+0x1F2/+0x1F6. **Commander**: the engine's own rule compares the unit's name with the
  side's commander name (`engine/units.md:1051`); the FBI `commander` bit (+0x245 0x40000) has
  false positives (ProTA ARMTHOVR). The class rule takes the name rule **or** showplayername with
  hidedamage (TADR's), so a decoy commander looks like a commander to everyone; the name rule alone
  would expose decoys at icon level.
- **Categories** (for mod icon sets, which key icons by FBI category tag) are not in the def: each
  category is a bitmask over UnitTypeID in a sorted name map at `0x51E6B4/B8`. **`0x488C50`
  inserts and allocates on a miss** — it must not be called with a mod's tag; a read-only walk of
  the map is needed.
- **Visibility**: unit +0x110 bit 8 radar (sight sets it too), bit 9 sonar, bit 10 jammed; the
  engine's radar list at main+0x14363 (10-byte entries, count +0x1436B), rebuilt every tick by
  `0x466DC0` — TADR's megamap draws from it. **Stock draws a radar contact on the minimap alone**
  and picks one there alone ([ui](engine/ui.html), *The minimap, located*; [input](engine/input.html),
  the `0x48CD80` row), so BAR's "radar-only enemies are always icons" would change 1×. Ours are the
  minimap's own dot under the icons' distance rule, not pickable in the world view, decided from
  the packet's +0x110 for the units the gather's cloak and fog tests refuse — no copy of the radar
  list. BAR's "real icon once identified" and out-of-radar ghost buildings have no TA counterpart.
- **Selected** +0x110 bit 4; **hovered** main+0x2CBA; **team colour** PlayerInfo+0x96. The minimap
  dot is frame PlayerInfo+0x96 of `radlogo`, fetched by `0x4B7F30` (the frame pointer, bounded by the
  sequence's count), and its team colour is the frame's hotspot texel [MEASURED on the retail
  `fx.GAF`].
- **The packet** carries +0x241 per unit but not bmcode, YardMap, weapons, +0x245 or categories:
  the class is computed on the game thread at the level's first in-play fill, per type, and the
  packet carries the table.
- **Picking an icon**: BAR widens the unit's pick radius to the icon (`iconRadius`,
  `UnitDrawerData.cpp:391`). The engine picks by model bounds through HotUnits/`GetUnitAtMouse`;
  **HotUnits holds 10 × MaxUnits ids, one per culled slot** (`0x485596`, `0x48BC6A`), so a wider
  pick changes what the hit test answers, not how many units are asked. Ours answers a hidden-model
  unit by its icon's circle at the hit test's one call `0x48CE15`
  ([strategic icons](modules/icons.html), *The pick*).

## Mod icon sets (TADR's format)

Every TADR mod sets `MegaMapConfig=.\Icon\iconcfg.ini` (TA Zero: `.\ZIcon\`) in its TADR ini
(`mayhem.ini`, `ProTA.ini`, `TAESC.ini`, `TAZero.ini`, `TA.ini`/`totala.ini`). The format
(`vendor/TADR/src/DDraw/UnitMinimap.cpp:700-985`):

- `[Option]`: palette indices `FillColor` (→ team colour), `TransparentColor`, `SelectedColor`
  (shown only when selected), `HoverColor`; `UseCircleHover`; `UseDefaultIcon`.
- `UseDefaultIcon=TRUE`: fixed file names in the folder — `COMMANDER`, `CONS`, `FACTORY`,
  `BUILDING`, `AIRCRAFTCOMBAT`, `AIRCONS`, `MOBILECOMBAT`, `NONE`, `UNKNOWN`, `NUKEICON` (`.PCX`),
  plus `<side>.PCX` per side (ARM, CORE, GOK). Mayhem, TA Zero and Twilight ship this, without a
  `COMMANDER.PCX`.
- `UseDefaultIcon=FALSE`: `[Icon]` maps an FBI category tag to a PCX; `Unknow`, `Nothing`,
  `NukeIcon` are special keys. ProTA 4.8 ships 24 entries: the three specials and 21 categories
  (AIR, AIRCONS, AIRFAC, BUILDING, EXP, EXPFAC, HOVER, HOVCONS, HOVFAC, KBOT, KBOTCONS, KBOTFAC,
  MINE, SHIP, SHIPCONS, SHIPFAC, SUB, SUBCONS, VEHICLE, VEHCONS, VEHFAC).
- **Escalation** ships a set in custom mode in its TAF install and its RAR — nine categories, AIR,
  LAB, BLDG, BIG, LRG, FAT, AIRCONS, CONS, BIGCON; the escalation-gold-10.2.0 fixture's `TAESC.ini`
  names `.\Icon\iconcfg.ini` but has no such folder. **Retail** has no TADR ini → the generated set.
- The pictures are 6 to 20 px square (TADR's bound is 22); each folder's `smallcircle`, 100 × 10,
  is TADR's hover circle and no icon. Every set and its sizes: [the setups](compat/setups.html),
  *The mega map's icon sets*.

The icons are read from the player's install at run time and never shipped, so the
redistribution question does not arise. TADR itself is never loaded (the takeover), only its
ini and art read. **The ini TADR reads** is the one named by the exe's string at `0x5098A3`
(`TAIniStr`, `HardCodeFunctions.cpp:799`), which the Community Patch Loader rewrites at start; its
`[Preferences]` `MegaMapConfig` names the icon config relative to the current directory, cut at its
last `;` (the research for landings 3–4, `.data/mega-map/facts-2026-11-03-icons.md` §E–§F in the
main checkout). Impure resolves it against the exe's folder, refuses an absolute one and cuts a
value at its first `;` ([strategic icons](modules/icons.html), *Mod icon sets*).

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
- **Stated, not fixed**: 5120×1440 on a 20 480-px map
  stops at ~0.083 (int16 clicks; the edges carry no margin); 640×480 on maps whose extent is
  taller than 26 624 px stops at 1/64.

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
1c. **The floor follows the map.** *Built 2026-11-03 on this worktree; the in-game checks are
   open* (spec v2 §4.6: the floor line and the rect at 1024×768 and 4K on Seven Islands, click and
   band-box selection and a move order at the floor, a hover at the screen's edge with the hit
   guard's count, the anchor and model counts at 1× and at the floor, the `vk: VA` line, the
   small-map regression, a format-26 recording, and the replay gate). A runtime floor
   `min(0.25, fit)` bounded by the int16 click rect, one predicate with vpwide's refusal; the seven
   guards take 1/64; the tables' reach follows the level through the ladder and the ring; the
   hit-test guard at `0x48CE15`; recording format 27 carries the camera down to 1/64, with the
   perspective test group turning a mega notch at tick 1080. What the spec v1 planned and v2
   dropped: the per-pass fog caps and the wide grid at the level (1a made them unnecessary) and a
   level-load hit-test margin (the guard replaces it). Not yet read off the game: the floor
   numbers above are arithmetic, and no `zoom: hit guard` count or `reachheld=` has been seen.
   Review at `high` (sync between the threads, a new byte patch, the recording format).
2. **The mega map binding.** A modifier bit in the notch slot; Ctrl+wheel back engages, wheel
   forward leaves at the pointer at the remembered level; legacy controls as an `impure.cfg` key
   and a menu row. Replay Follow and the file lever keep their priority.
3. **Strategic icons, BAR's rule.** *Built 2026-11-03 on its worktree; the in-game checks are
   open* (the spec's V3-1..V3-8, `.data/mega-map/landing3-4-spec.md`: 1× identical to the stock
   frame, every class at the floor, the fade band's alphas, command on icons, radar dots, the 4K
   numbers, the cost, the keys). The module: [strategic icons](modules/icons.html). A class per
   type, built on the game thread at the level's first in-play fill and carried in the packet;
   generated shapes by class, drawn by the marker pass with a texture kind, an atlas and an FS
   branch of their own (the order art's path is fogged and single-coloured, so it was not reused);
   the model, its shadow, its selection rect, health bar and group digit not drawn while the icon
   covers it, and a hidden unit's cargo neither a model nor an icon; radar contacts as the
   minimap's dot under the same distance rule, from the packet's +0x110; selected white; BAR's
   order (layer, then far to near); the pick by the icon's circle at `0x48CE15`. A 7-px floor on
   the drawn edge, the smallest at which the nine classes are pairwise different, with the switch
   kept on BAR's unfloored size. Three closed-list `impure.cfg` keys — `iconscale`, `icondist`
   and the fade band `iconfade` (default 33, so 1× stays stock up to a 2560-px-tall frame) — and
   **no menu rows**: the front-end screen has no free slot and three rows would run the in-game
   panel 46 px past 640×480, so rows wait for a panel layout that fits. The class table's and the
   art's game-thread code sits in `tagpu_packet_pub.c`, an existing publisher, so
   `thread-split.allow` gains no line.
4. **Mod icon sets.** *Built 2026-11-03 on its worktree; the in-game checks V4-1..V4-5 are open*
   (`.data/mega-map/landing3-4-spec.md`: Mayhem's set at the floor in team colours and selected,
   ProTA's categories and its UNKNOWN, Escalation's set from a full install and the compat setups'
   Escalation with no set, TA Zero's `ZIcon` and retail with none, the pick on set pictures). The
   module: [strategic icons](modules/icons.html), *Mod icon sets*. Once a process, at the first
   in-play fill and before the art's release store, the TADR ini named at `0x5098A3`, its
   `MegaMapConfig`, the iconcfg and its PCX pictures are read from the install as bounded files and
   parsed by pure, host-tested code (`tagpu_iconset.c`); the pictures go into the icon atlas beside
   the radar logo, FillColor → team colour (white when selected), TransparentColor → nothing,
   SelectedColor shown only when selected, in the set's own colour. Once a level, each type takes
   the first picture in TADR's load order whose types hold it — TADR's rules, with the categories
   from a read-only walk of the engine's map `[0x51E6B4, 0x51E6B8)` that never calls `0x488C50` —
   else the set's UNKNOWN, else its generated shape; a radar contact draws the set's NONE. Both
   threads size a set picture through one function, so the pick's circle is the picture's. The
   game-thread code sits in `tagpu_packet_pub.c`, so `thread-split.allow` gains no line.
5. **Documentation** rides with each landing; the last one adds an in-game wiki entry.

Presentation only: nothing here writes simulation state. The recorder question in landing 1 is
the one place it meets the replay.
