# The in-game wiki's stage in the game's own 3D graphics, planned

**Written** 2026-10-26, before any of it is built. It replaces landing 6 of
[the wiki's plan](ingame-wiki-plan.html), the sprite baker it scoped, which the owner declined. The
specification is still [the in-game wiki](ingame-wiki.html) and its mockup. Every engine fact
carries its address and how it was established: DISASSEMBLED (from `pristine/TotalA.exe.pristine`),
cited from [the engine map](exe-reverse-engineering.html), read in our source, or [INFERRED]. Like
its parent, this file is folded into the wiki's note, `gpu-status.md` and `roadmap.md` when the work
closes, and then deleted.

## The owner's decisions (2026-10-26)

1. **The stage is a real 3D render, not sprites.** A second, small world render: its own offscreen
   target and its own camera at the stage rect, drawn before the wiki layer, which then shows it
   as an image. It reuses the game's terrain, feature and unit passes rather than a software baker.
2. **The wiki's units become model records**: model, rest pose, position, heading. The simulation
   gains a heading, in `sim.js` and in C together, with parity kept.
3. **The ground is a small custom map**: a corner of real maps' tiles, with the features the demos
   need (metal deposits, a geothermal vent, wrecks where a demo wants one).
4. **Units load from the game's files**, not from the level's loaded templates, found at their real
   path in the running mod's archives and folders.
5. **What is committed is our own description of the stage map** (question 2, answered the same
   day): which tile goes where, by reference to a source map and a tile index, and which feature
   sits where, by name. The art is looked up in the player's archives when it loads. Nothing
   derived from the game is committed, and nothing is cut into a cached TNT at run time. A missing
   map, tile or feature falls back by construction, never crashes.

## Where it starts

- **On the wiki branch at `848309b7`**: the core (`tagpu_wikisim.c`, `tagpu_wikidata.c`), the
  shell (landing 2: button, window, capture, pause), the page's layout (`tagpu_wikilay.c`) and the
  stage's ops (`tagpu_wikistage.c`, `stagecheck.mjs`) are committed. The coordinator reports
  landings 3 and 4 (the page and the whitebox stage drawn in the wiki layer) built; at `848309b7`
  `tagpu_wiki.c` and `tagpu_wikistate.c` do not call the stage or the simulation yet. This plan
  assumes landing 4 as [the parent plan](ingame-wiki-plan.html) specifies it: the fill copies a
  `WIKISTAGEVIEW` into the record, `tagpu_wikistate.c` builds `WIKISTAGEOPS` on the render thread,
  and `tagpu_vk_hud.c` draws them in the wiki layer. If landing 4 lands with other names, 6g below
  takes them; nothing else changes.
- **The mockup's `gfx=game`** draws sprites that `tools/wiki/gfx.py` bakes in Python. It is
  replaced, not kept beside the 3D stage (one look, no "old mode").
- **`tools/tascene`** already builds a scene pack from the player's archives (tile atlas, tile map,
  feature atlas and anchors, unit meshes, palette, the SHD table) and `tools/tascene-view.html`
  draws it in WebGL with tagpu's own shaders in its parity lane, MEASURED against the game at zoom 1:
  terrain 52 differing pixels of 354 744, sprite bodies identical
  ([tascene](tascene-design.html), *The A/B, measured*).

## The engine facts this rests on

### How the engine opens a file

- **`0x4BB2E0(path, mode)` tries the loose file first, then the archives in mount order, and the
  first archive that has it wins.** It calls the C runtime's `0x4E4990(path, mode)` on the path as
  given, relative to the process's current directory (`0x4BB32D`); on success the handle's `+0`
  is the `FILE*` and `+4` is 0 (`0x4BB339..0x4BB347`). Otherwise it walks the mount list
  `[0x4B6220()+0x618]` from index 0 to `+0x61C` (`0x4BB34A..0x4BB387`), asks each archive's root
  directory (`[[handle+8]+0x10]`) through `0x4BB4E0`, and stops at the first entry whose flag bit 0
  (directory) is clear (`0x4BB378`). An archive whose `FILE*` was closed is reopened from its path
  `+0x14` (`0x4BB3A7..0x4BB3D4`). DISASSEMBLED 2026-10-26.
- **`0x4BB4E0(dir, path)` splits the path at `\` only** (`push 0x5C`, `call 0x4E65C0` at
  `0x4BB4EA`), scans each directory's 9-byte entries from the last to the first
  (`0x4BB530..0x4BB558`) comparing names with `0x4F8A70` [INFERRED: a case-insensitive compare;
  not disassembled], and descends into an entry with flag bit 0 (`0x4BB56C..0x4BB57E`).
  DISASSEMBLED 2026-10-26.
- **The mount list and its order** (cited, *Where the engine looks for archives*): the graphics
  globals `*(0x51FBD0)` hold the handles at `+0x618` and the count at `+0x61C`; each handle's
  `+0x14` is the archive's full path (`OpenHAPIFile 0x4BDD70`); `InitTAHPIAry 0x41D4C0` mounts
  `rev31.GP3`, `*.CCX`, `*.UFO`, `*.HPI` in that order (`0x41D4E0..0x41D5AB`), then `%c:\*.hpi` per
  drive; only it mounts, from eleven callers, all on the game thread; the list is final by a
  skirmish's first iteration, and RESTART (`0x460428`) or the load dialog (`0x49257A`) can still
  add to it.
- **Enumerations list loose files first, then the archives in mount order**: the unit definitions
  (`0x4BCA30`, the def array's order) and the feature TDFs (`0x4BCB50("features", "*.tdf", −1, 1)`,
  recursive; LoadFeature `0x4224B0` takes a section from the first TDF that holds it). Cited.
  What an enumeration does with one path present in two sources is QUESTIONS batch A1.

### What a mod changes

- **A mod's executable renames the first archive pattern, so its own archive mounts first and wins
  every path it holds.** The main-menu classifier compares the exe's `rev%s.GP3` string
  (`VA_STR_ARCHIVE`, `0x5028CC`) and its ini name against a table: Escalation `TAE%s.GP3` /
  `taesc.ini`, Total Mayhem `may%s.GP3` / `mayhem.ini`, ProTA `ProTA.gp3` / `ProTA.ini`, TA Zero
  `TAZ%s.GP3` / `tazero.ini`; Twilight is told by its main menu's label
  (`tagpu_replays.c`, `s_mods` and `mainmenu_visit`, from the source; the strings were read from
  the compatibility fixtures). That classifier is what sets `tagpu_profiles_mod()`.
- **No note records a mod moving `units\`, `objects3d\`, `scripts\` or `textures\`** [INFERRED];
  QUESTIONS batch D lists the engine's directory strings and compares them across the fixtures.
- **A mod can lack what the stock game has**: Mayhem's `mayhem.gp3` carries 105 maps that are only
  `.ota`, 9 of them with a terrain nowhere ([compat setups](compat/setups.html)); TA Zero plays its
  own sides.

So **a reader that asks the engine's question of the engine's list, in the engine's order, finds
what the running game would find, whatever the mod**. `tools/wiki/gfx.py` does not: `ta3do.Assets`
lets the last archive win and `gfx.py` then forces `rev31.gp3` to win, which is wrong for every mod
above.

### Models and frames in memory are the files, rebased

- **Every field the DLL reads of a `Model3DONode` sits at the 3DO file's own offset**:
  `tagpu_model3do.h` (`N_VCOUNT 0x04`, `N_FCOUNT 0x08`, `N_SELPRIM 0x0C`, `N_OFF 0x10`, `N_NAME 0x1C`,
  `N_VERTS 0x24`, `N_FACES 0x28`, `N_SIB 0x2C`, `N_CHILD 0x30`; faces of `0x20` with `F_VCOUNT 0x04`,
  `F_INDICES 0x0C`) against the file's object and primitive structs (`tools/ta3do` `parse_3do`).
  The face's `+0x10` (the file's texture-name offset), `+0x18` and `+0x1C` are what the engine
  rewrites into `F_TEXFRAME`, `F_TEXSEQ` and `F_FLAGS`. **So the engine's template is the file with
  its offsets rebased to pointers and its textures bound** [INFERRED from the coincidence of every
  offset; QUESTIONS A4 confirms which fields the loader writes and the binding rule].
- **A GAF frame and sequence likewise**: `TAGPU_GF_*` (w, h, hotspot, key `+0x08`, compressed
  `+0x09`, sub-frame count `+0x0A`, pixels `+0x10`) is the file's 24-byte frame-data struct, and
  `TAGPU_SQ_*` (count `+0x00`, name `+0x08`, table `+0x28` stride 8) the file's entry
  (`tools/ta3do` `gaf_frame`, `gaf_entries`) [INFERRED the same way; QUESTIONS A6].
- **The readers' range filters accept any pointer in (0x600000, `tagpu_user_top()`]**
  (`ptr_ok` in `tagpu_posebake.c`, `tagpu_render3do.c`, `tagpu_gaf.c`; `tagpu_m3_ptr_ok` in
  `tagpu_model3do.h`; from the source).

Together: **a model loaded from its file the way the engine loads it, into memory inside that
range, is read by `tagpu_posebake.c`, `tagpu_render3do.c` and `tagpu_gaf.c` exactly as an engine
template is**, with no line of theirs changed.

### The world passes, and what ties them to one scene

The Vulkan passes read no engine state; every value arrives in a hand-over built earlier in the same
render-loop iteration (`tagpu_vk_pass.h`). What ties each to the one game scene (read in the source
2026-10-26):

| Pass | Its hand-over source | Per-pass state a second scene would collide with |
|---|---|---|
| `tagpu_vk_terr.c` (2 064 lines, ~53 statics) | `tagpu_terr_handover`, one a frame | per-slot uniform and instance buffers and descriptor sets (`s_slot[]`); one shared base atlas and height grid, retired behind a per-slot visit mask; the restorer job; latches |
| `tagpu_vk_feat.c` (1 729, ~45) | `tagpu_feat_handover`; the atlas protocol `tagpu_feat_atlas_hand/owed/ack` | per-slot buffers and sets; one base atlas; the atlas's single acked serial (`tagpu_gaf_atlas_ack`) |
| `tagpu_vk_unit.c` (4 007, ~73) | `tagpu_posedraw_handover`; vertices by `tagpu_posebake_geom_mirror/mat_mirror` | per-slot pose and uniform buffers and sets; the vertex-buffer cache keyed on bake serials; one unit atlas copy and its ack (`tagpu_posedraw_atlas_ack` → `tagpu_r3d_atlas_ack`); the effects-model hold; the shadow and scaffold bindings |
| `tagpu_posedraw.c` (1 364) | called by `tagpu_native.c`; reads `tagpu_native_foggrid`, `tagpu_native_scissor_on` | one recording window and one hand-over a frame; its arenas |
| `tagpu_posebake.c` (980) | PK_PIECE runs naming template nodes | caches sized for the game's frame (`TAGPU_PB_FRAMEMAX` = units + wrecks + builds + 1) with the "never evict what this frame asked for" bound; latched level and atlas generations |
| `tagpu_render3do.c` (577) | face records and GAF frames | one unit atlas (`s_atlas`), its per-frame paint allowance and its single consumer's ack |
| `tagpu_vk_world.c` (1 293) | `tagpu_native_worldtgt` (`gw`, `gh`, `ss`, the destination rect) | one colour+depth target per slot; the composite into the viewport rect, with the engine's Gamma curve (`tagpu_pal_gamma`) |

- **The camera is already a parameter.** Every pass scales the game-space rect it is handed into
  whatever extent it records into (`gw`, `gh`, `zoom`, `zoomC`, the viewport rect and `ss` are
  hand-over fields; `terr_scissor`, `tagpu_vk_world.h`), and TA's camera is the fixed oblique shear
  `screen y = world z − height/2` ([tascene](tascene-design.html), *Decisions locked*). A second
  camera is a second set of numbers, not new code.
- **Palette and fog are hand-over fields** (`pal`/`palSerial`, `fogGrid`), except that
  `tagpu_posedraw.c` asks `tagpu_native_foggrid` directly.
- **A second scene through the same instances would be corrupted, not merely wrong**:
  - a second `prepare` on the same slot overwrites mapped uniform, instance and pose buffers that
    the first scene's draws, recorded in the same command buffer, have not read yet;
  - the hand-overs are taken exactly once a frame and stamped with it;
  - a shared atlas tracks one consumer's acked serial, and its allowance (no more tiles due than
    `TAGPU_VK_STAGE_CAP` holds) is an argument about that one consumer
    (`tagpu_vk_stage.h`, `tagpu_gaf.h`);
  - posebake's eviction bound counts one scene's records;
  - `tagpu_gaf.c` decodes through static buffers (`s_dec`, `s_pad`), so every paint must stay on
    the render thread.

**MEASURED 2026-10-26**, `size` on the branch's i686 objects, `.bss` in bytes: `tagpu_posebake.o`
9 925 088 (its two scratch streams for one model at `PB_MAXVERT` 49 152 vertices are 5.3 MB of it),
`tagpu_vk_unit.o` 2 854 080, `tagpu_gaf.o` 881 312, `tagpu_render3do.o` 188 928,
`tagpu_vk_feat.o` 34 400, `tagpu_posedraw.o` 2 656, `tagpu_vk_terr.o` 2 304, `tagpu_vk_world.o`
1 408.

## The design

### 1. The second scene: the per-scene modules compiled twice

**Decision.** Seven files are compiled a second time, with `-DTAGPU_SCENE_WIKI`, into a second
instance with its own statics: `tagpu_vk_world.c`, `tagpu_vk_terr.c`, `tagpu_vk_feat.c`,
`tagpu_vk_unit.c`, `tagpu_posedraw.c`, `tagpu_posebake.c` and `tagpu_render3do.c`. The game's
instance is the existing object, unchanged. A new header, `src/tagpu_scene.h`, included first by
each of the seven, does four things under `TAGPU_SCENE_WIKI` and nothing otherwise:

1. **Renames every external symbol the seven define** (`tagpu_vk_terr_prepare` →
   `tagpu_vkw_terr_prepare`, `tagpu_r3d_atlas_uv` → `tagpu_r3dw_atlas_uv`, …), so calls between the
   wiki instances stay among them.
2. **Points the hand-over sources at the wiki's producer**: `tagpu_terr_handover`,
   `tagpu_feat_handover`, `tagpu_feat_atlas_hand/owed/ack`, `tagpu_native_worldtgt`,
   `tagpu_native_foggrid`, `tagpu_native_scissor_on` → `tagpu_wikiworld_*`.
3. **Points every call into a game-only module at an inert answer**: the shadow map, the scaffold,
   the effects pass, the Classic++ restorer and its switches (`tagpu_classicpp_on`,
   `_assets`), and the A/B lever answer "none" (`tagpu_wikiworld.c`'s stubs; the wiki's
   hand-overs carry `shadowOn`, `scafOn`, `restored` 0 and no effects records). The wiki
   instances therefore bind no image a game pass writes and queue no restorer job.
4. **Sizes the wiki instances for the wiki**: `TAGPU_PB_FRAMEMAX` and the arrays it sizes in
   `tagpu_posebake.c` and `tagpu_vk_unit.c` come from the stage's bounds
   (`WIKIWORLD_FRAMEMAX` = `WIKISTAGE_UNITS` + `WIKISTAGE_SITES` + 1 + `WIKIWORLD_FEAT3D`, the
   description's 3D features, 64, which the description's reader refuses to exceed: 449). The two
   keeps are set to the bound as well, since a keep above its table would let the table fill
   without ever recycling (`tagpu_posebake.h` asserts `keep <= table`), and the two lookup hashes
   over those tables (`PB_HBITS`, `VB_HBITS`) to 2 048 chains. posebake's two scratch streams, its
   skip flags and its normal scratch (`tagpu_pb_scratch_*`) are defined in the game's object only
   and are `extern` in the wiki's (a `PB_SCRATCH` macro): one bake runs to completion inside one
   call and both instances run on the render thread, so they never use it at once.

**The invariant, stated where it is enforced.** *No static of the wiki's stage is a static of the
game's world*: two translation units share no file-scope state, which C's linkage guarantees for
every static these files hold today **and for every one a later edit adds**. *No wiki instance
reaches a game-only module*: `tools/scene-check.sh`, an order-only prerequisite of the link like
`thread-split-check.sh`, lists each wiki object's undefined symbols (`nm -u`) and fails on any
outside an allow list of stateless or read-only helpers (`tagpu_vk_stage_*` over a caller's
`TAGPU_VKSTAGE`, `tagpu_gaf_*` frame readers and atlas functions over a caller's atlas, `tagpu_pal_*`
reads, the device queries `tagpu_vk_armed`, `tagpu_vk_owns_present`, `tagpu_vk_max_image_dim` and
`tagpu_vk_max_storage_range`, `tagpu_line_*`, `tagpu_log*`, `tagpu_settings_ss`, the C library and
the Win32 and Vulkan imports), naming the symbol. As built (6f): the C library and the imports are
recognised as the undefined symbols no object of ours defines, the `tagpu_gaf_*` and `tagpu_pal_*`
entries are the precise families and reads the passes call, `tagpu_user_top_bound` (the range
filters' top, written once at attach) is on the list, and the bake's shared scratch is too. The
check reads objects, so it is a phony target that depends on every object of the link and the link
waits on it (order-only), which cannot race the compiler. The shadow map's format query is not a
device query to allow but one of the stubs: its `VK_FORMAT_UNDEFINED` is an answer both passes
already take, giving their dummy their own depth format. The rename list is proven complete by
the link itself: a symbol left unrenamed is defined twice. **The seam's declarations of the
renamed entry points are 6g's, and not by re-including the headers**: each of the seven headers
is include-guarded and defines types (`TAGPU_PDHAND`, `TAGPU_R3DTEX`, the unit pass's records)
beside its prototypes, so a second inclusion in `tagpu_vk.c` under the renames is either skipped
by the guard or redefines the types. 6g separates each header's prototypes from its types, or
declares the wiki's entry points by another construction that keeps their signatures the
originals', so `tagpu_vk.c` still calls the wiki's instance by name only.

**What each instanced file gains**: the include, and a paragraph at the top of its header comment —
"TWO INSTANCES: compiled for the game's world and, under `TAGPU_SCENE_WIKI`, for the wiki's stage,
each with its own statics; `tagpu_scene.h` names where the wiki's draws its hand-over from and
what it may call". Nothing in the bodies changes, except posebake's scratch macro and the two
hash sizes' `#ifndef` (posebake's paragraph adds that the scratch is the one shared state). Four of the
seven carry GLSL that `tools/spirv-gen.py` extracts (`tagpu_posedraw.c`, `tagpu_vk_terr.c`,
`tagpu_vk_unit.c`, `tagpu_vk_world.c`): the build's `spirv-check` says whether the header edit
needs a regeneration.

**What it costs**: about 250 KB more code in the DLL; the wiki instances' `.bss` small once
resized; device memory for the wiki's own atlases and per-slot buffers, sized by its content (a
few MB). MEASURED 2026-10-26 (6f): the DLL +262 144 bytes, `.bss` +363 296, under the 400 KB
target (`gpu-status.md` §2.12d has the per-object figures); device memory is 6g's to measure.

**Considered and declined:**
- *A per-scene state struct in each file.* About 170 statics across the seven files and every
  reference to them in about 12 000 lines change, on the path every frame of the game draws; and a
  later static added at file scope would be shared by both scenes silently. The invariant would be
  a convention, not a construction.
- *Sharing posebake, render3do and the unit atlas between the scenes.* The atlas's ack and
  allowance are a one-consumer argument (above), the eviction bound counts one scene, and the
  wiki's draws would depend on the game's frame sequence (`tagpu_native_frame` resets the allowance
  and recycles the atlas).
- *A dedicated wiki pass that reuses only the SPIR-V.* It would re-implement each pass's vertex and
  uniform packing, which is where these passes are subtle, and drift from them.

### 2. The stage camera and target: the game's own scale, composited into the map rect

- **The wiki scene renders the map at zoom 1, one world unit a game pixel, at the game's own `ss`**
  (`tagpu_settings_ss`): the target is `mw·16·ss` × `mh·16·ss` (1280 × 768 for the 40 × 24-cell map
  at `ss` 2; at most 4 096 for `WIKI_MAP_MAX` 128, under `WORLD_MAXDIM` 8192). The hand-overs carry
  `gw = mw·16`, `gh = mh·16`, `zoom` 1, the eye at the map's top left, the viewport rect the whole
  map with the scissor on, no mirror past the edge.
- **The wiki instance of `tagpu_vk_world.c` composites it into the map's rect on the stage canvas**
  (`TAGPU_WORLDTGT` `vx, vy, vw, vh` = the canvas corner plus `tagpu_wikistage_fit`'s `ox, oz`, and
  `mw·16·s` × `mh·16·s`), with LINEAR and the engine's Gamma curve, as the world's composite puts
  the world on the screen.
- **Why this camera**: the image is the game's own picture of that map, the same at every screen
  size, and zoom 1 is the only scale at which the parity lane carries a pixel claim — so the image
  diff against the mockup compares fixed-size images at the scale the game is exact at.
- **The ground is flat** (height 0 everywhere; the authoring tool takes flat tiles only), so the
  ground plane under the shear is the whitebox's top-down grid: **the composite rect and every
  overlay op come from the one fit**, `tagpu_wikistage_fit`, and cannot disagree. A model taller
  than its row's distance to the map's top edge is cut there, as at the game's viewport edge.

### 3. What the image holds, what stays an op, and the fallback

- **In the image** (drawn by the passes): the terrain, the features (deposits, vents, 2D features;
  3D features such as wrecks through the unit pass, as the game draws them), every unit at rest
  with its heading and its construction state (the engine's nano staging,
  `tagpu_r3d_nano_state`), the Classic shadows under the engine's options, and the placement ghost
  and each gesture site's ghost (alpha 0.40, depth writes off, after the units: the game's ghost
  stage). The ghost is untinted, as the game draws it; its colour is its square's.
- **Over the image, as ops** (the game's marker layer is over the world too): order lines, queued
  squares, selection brackets, the gesture sites' and the ghost's squares and the sites' order
  numbers, the drag box, the cursor, the readouts, and the whitebox box and label of any unit whose
  model did not resolve.
- **Three op ranges a frame**, all built by the pure stage builder on the render thread:
  - **R0**, under the image: the window, the page and the stage's background;
  - **R1**, the whole whitebox stage, as today;
  - **R2**, the overlays above.

  The wiki layer draws **R0, the image, R2** when the image is complete this frame, and **R0, R1**
  otherwise. Both choices are made at record time from the seam's own facts, so no frame shows half
  of one and half of the other.
- **Complete means**: the wiki target was prepared, and every wiki pass whose content this frame
  expects (`terrain` when the ground resolved, `features` when there are any, `units` when any
  record resolved) returned 1 from its `prepare`, and every unit and ghost record of the frame
  baked (the producer counts the refusals, as the game's `otherDraws` does). A unit pass that
  refuses for device memory therefore turns the stage whitebox, never empty.
- **The ground falls back whole.** If any tile or deposit feature of the stage's description fails
  to resolve, the image carries no terrain and no features, R1's ground is drawn, and fog is the
  whitebox's. A partial ground could show a rule without its picture (a deposit the simulation
  has, with grass over it), so there is none. **Units fall back per type**: a type whose model did
  not resolve has its whitebox box in R2; the others are 3D.

### 4. The stage description: committed indices and names, resolved against the player's archives

**One file per entry's scene**, `art/wiki/stage/<entry>.json`, keyed by the entry's id (the entry
files do not change). Packed into `WIKIPAK` by `tools/hudpak.py --set wiki` from the landing that
reads it (6g).

```json
{ "format": 1, "entry": "quickbuild",
  "cells": [40, 24],
  "sources": [ { "map": "maps\\Crossing Two Rivers.tnt", "tiles": 4096 },
               { "map": "maps\\<a metal world map>.tnt", "tiles": 2310 } ],
  "tileset": [ [0, 13, "9f3a17c2"], [1, 402, "c0d11e88"] ],
  "tilemap": [ 0, 0, 0, 1, … ],
  "features": [ { "name": "lushmetal3", "cell": [14, 16] },
                { "name": "lushvent01", "cell": [28, 9] } ] }
```

- `cells` must equal the scene's map; `tilemap` is `cells/2` tiles a side (a TNT tile is 32 world
  units, two cells), each an index into `tileset`.
- A `tileset` row is `[source, tile index in that TNT, FNV-1a 32 of the tile's 1024 bytes]`; a
  source's `tiles` is its TNT's tile count. The hash and the count are identity checks
  (question 4): a map of the same name with other tiles is refused, not drawn as a patchwork.
- A feature is a TDF section name at a cell (the feature's anchor cell, as LoadFeature places it).
  A 3D feature (`object=` in its section, a wreck such as `armmex_dead`) is drawn through the unit
  pass.
- **No art, no pixels, no palette, no extracted file**: indices, names, counts and hashes only.

**Authoring: `tools/wiki/stagemap.py`**, run with the shared venv against the local install
(`--gamedir`, else `TA3DO_GAMEDIR`, as `gfx.py` finds it), reading through the resolver of
section 5's rules in Python (`tools/ta3do`'s HPI and GAF readers, `tools/tascene`'s TNT and feature
readers):
- `stagemap.py tiles <map>`: the map's flat, featureless, tileable tiles with their indices and a
  contact sheet into the gitignored `tools/wiki/mockup/gfx/` for the author to look at.
- `stagemap.py build`: the description from a recipe held in the script, as `gen-quickbuild.mjs`
  holds the demos — a ground tile, a metal-ground tile over the scene's metal-ground rects, a
  deposit feature centred on each deposit, a vent feature on each vent cell — writing only the
  JSON above.
- `stagemap.py check`: resolves every description against the install and checks it against its
  entry's scene: every map and tile found with its count and hash; every feature found; each
  deposit feature's footprint over a metal rect of the scene and each vent feature on a vent cell;
  every metal-ground rect and fog rect of the scene aligned to whole tiles (even cells; question 5).
  Exit 1 naming each failure.

**Resolution in the DLL** (6e, on the 3D loader thread): each source TNT opened by the engine's
rule (section 5), its header bounds-checked, its tile count compared, only the referenced tiles
read and hashed; each feature section found by the engine's enumeration rule and its GAF frame or
3DO loaded. **Resolution in the mockup** (6d): the same rules in Python, writing the gitignored
pack. **A miss** — no such map, another tile count, a hash that differs, no such section, a frame or
model that does not load — is recorded per element in the arena's report, logged once, and the
ground falls back whole (section 3). Nothing indexes a tile set or a feature table with an
unchecked value.

**Considered and declined**: cutting a corner of a retail map into a TNT at the first open and
caching it on disk (the owner's decision 5).

### 5. Units from files: a DLL-side reader asking the engine's question

**Recommended: a DLL-side archive reader over the engine's own mount list, not a call into the
engine's loader.**
- **Why not the engine's loader.** Its file layer is the engine's: the handles' seek state is
  shared with the engine's own reads, so it may run only on the game thread, where a load of models
  and textures would stall a frame (or many, under a mod with a thousand FBIs). It loads into
  engine structures with level lifetimes. And it needs new `publisher` lines in
  `thread-split.allow`.
- **Why the reader is safe.** It opens its own read-only handles, on its own thread, of the files
  the engine mounted. It reads nothing of the engine's but one copied list.
- **The DLL has no archive reader today**: `tagpu_ufo.c` writes one; `tools/ta3do` has the Python
  reader to port; `lodepng.c`'s `lodepng_zlib_decompress` covers SQSH mode 2.

**The rules, which are the engine's** (DISASSEMBLED or cited above; the open points are QUESTIONS
batch A, and the reader implements whatever the scouts establish without changing this design):
- **A path** opens loose first, relative to the process's current directory as the engine's
  `fopen` would, then from the first archive in mount order whose directory holds it as a file,
  the path split at `\` and compared without case [INFERRED for `0x4F8A70`; A1].
- **The archive list** is the engine's own: at the wiki's first open, on the game thread, the open
  copies every mounted archive's full path (handle `+0x14`, bounded by `+0x61C` and by `0x104`
  bytes each) and the current directory by value, and hands them to the loader thread. The read is
  `tagpu_replays.c`'s, which already reads the same list in `running_mod` (question 2).
- **A unit name** is matched against `UnitName` over `units\*.fbi`, in the engine's enumeration
  order with its rule for duplicates (A1, A2). The FBI's `Objectname` names
  `objects3d\<objectname>.3do` (A2). The script `scripts\<name>.cob` gives Create()'s hides (A3).
- **Textures** are bound by the engine's rule (A4, A5) from the texture GAFs, read in the engine's
  order; the frames a model's faces name are copied into the arena, not whole GAFs.
- **The model is loaded as the engine holds it**: the 3DO blob copied into the arena, every offset
  bounded inside the blob and rebased to a pointer, every face's texture bound to a rebased GAF
  frame or sequence with its flags, the tree walked once with `tagpu_model_walk`'s bounds
  (`TAGPU_PBMAXPIECE`, `PB_MAXNODEV`, faces ≤ 512, a cycle refused).
- **The arena** is one `VirtualAlloc(MEM_TOP_DOWN)` block, refused whole unless it lies inside
  (0x600000, `tagpu_user_top()`], so every pointer in it passes the readers' range filters by
  construction. It is published by one interlocked store and never freed.

**"A mod alters the default paths" is handled by construction.** Nothing in the resolver names a
mod, a pattern or an archive: it reads whatever the engine mounted, in the engine's order, with the
engine's precedence, so it finds the copy the running game would draw — Escalation's ARMCK from
its own first-mounted `TAE…GP3` if it ships one, the stock one from `totala1.hpi` on retail. A unit the running game does not have
resolves to nothing, and its whitebox box is drawn.

**Mod detection** (`tagpu_profiles_mod`) is not part of resolution; it still decides only the
"Turned off in this mod" badge.

**Bounds on untrusted bytes** (every archive is data): the HPI header, key and directory offsets
against the file size; directory depth ≤ 16 and entries against the directory block; SQSH chunks
≤ `0x10000` out with the declared size checked after LZ77 or zlib; TDF and FBI text parsed within
its length; TNT offsets and tile indices against the file and its count; GAF dimensions ≤
`TAGPU_GAF_DECMAX`, RLE rows within the blob, sub-frame counts against the table; COB reads by
`create_hides`'s own bounds.

### 6. Model records and the heading

- **The simulation keeps a facing vector per unit, `(hx, hz)`, not an angle.** The two simulations
  agree bit for bit because they use only + − × ÷ and `sqrt` (the contract in the wiki's note); an
  angle would need `atan2`, whose last bit differs between engines.
  - **Movement**: a walk step sets the facing to the step's own `dx / d`, `dz / d`, the numbers
    `walk` already computes.
  - **Work**: a builder within range of what it builds or reclaims faces the target's centre
    (question 9).
  - **Buildings** face `(0, 1)`, toward the viewer: TA's default heading `0x8000`, the centre of
    the spawn's `0x8000 − BuildAngle/2 + rand(BuildAngle)` (`0x485A40`, cited in `tagpu_native.c`
    `ghost_one`).
  - **A scene unit** may give `"facing": [x, z]`, normalised with `sqrt`; the default is `(0, 1)`.
  - Both enter the state hash.
- **The record** gains, per unit and per site, the type's index into the entry's types (bounded by
  `WIKI_MAX_DEFS`), and per unit `hx`, `hz`; the stage part gains the entry's index and the
  simulation's tick (for the nano staging). All values, in `tagpu_wikipk.h`.
- **The render side builds the yaw from the vector**, with no angle: the body rotation is the one
  `ghost_one` builds for `0x8000` when the facing is `(0, 1)` (`diag(−1, 1, −1)`), turned by the
  vector's components. Its lateral sign is [INFERRED] and checked by a capture (6h): a unit walking
  toward +x faces +x.
- **A model record on the render thread** is: the type's resolved model (the arena's tree), its
  rest pose (`ghost_one`'s arithmetic: every piece turn 0, the body turn folded into the root),
  Create()'s hides (`tagpu_cobscan.c`, 6c), the anchor `(x, z)` at ground height 0, the depth row
  `1 + (z >> 4)·4`, alpha 1 and depth writes on (a ghost: 0.40 and off), the nano staging from the
  built fraction and the tick, the Classic shadow kind (a structure's slant, a mobile unit's
  silhouette, under the engine's option bits the packet carries), `lit` under the Shading option,
  and no fog (section 7). It goes through the wiki instance of `tagpu_posedraw_unit`, the same entry
  the game's units take.

### 7. Fog

- **Terrain and features take the engine's fog**: the producer builds the stage's fog grid in the
  engine's own form — 32-px cells, two 4-bit corner masks, unexplored and grey (`0x4843C0`;
  `tagpu_glsl.h` *TAGPU_GLSL_FOG_UNIFORMS*) — from the record's fog cells, with corners at the
  centres of 32-px blocks (`uFogOrg` (16, 16)). The scenes' fog rects are whole tiles (question 5),
  so each block is uniform, and a straight fog edge falls on the rect's edge exactly. A rect's
  corner is rounded, as the engine feathers it.
- **Units and ghosts take no fog** (`fog` 0): the player's own units are never fogged in the game,
  and the simulation has no line of sight, so a building raised under grey fog must stay visible.
- When the ground falls back, fog is R1's whitebox overlay, as today.

### 8. Threads and data

| State | Owner | Written by | Read by |
|---|---|---|---|
| the mount-list copy and the current directory | game thread, at the first open | `tagpu_wiki.c` through `tagpu_replays.c` | handed by value to the 3D loader thread, then its own |
| the 3D arena: tiles, the stage's R8 terrain atlas, feature frames and definitions, models with bound textures, Create() masks, the resolution report | published once by the 3D loader thread, immutable, never freed | the loader | the render thread (`tagpu_wikiworld.c`, the wiki instances) |
| the record's stage part, with types, facings, tick and entry | the packet | the game thread's fill | the render thread |
| the wiki atlases (a `TAGPU_GAFATLAS` for features, the wiki render3do's unit atlas), the wiki bake caches, the wiki posedraw arenas, the three hand-overs, the target's geometry | render thread | `tagpu_wikiworld.c` | the wiki pass instances, in the same render-loop iteration |
| the wiki instances' device objects | render thread | the seam's prepare and record | the device |

- **The hand-overs are produced and consumed in one iteration of the render loop, on one thread**:
  `tagpu_wikiworld_frame` runs in `tagpu_overlay_draw` right after `tagpu_wikistate_present`, which
  laid out this frame's stage canvas, and `tagpu_vk_frame` consumes them later in the same
  iteration. Each carries the frame's stamp and is refused on any other: the rule every hand-over
  here follows.
- **What the record carries is copied out before `tagpu_packet_frame_end`**: the producer builds
  into its own buffers.
- **The 3D loader thread** is started once, at the first open, guarded by an interlocked compare,
  with its inputs by value. It reads files only and publishes with one interlocked store. It
  touches no atlas and no device: painting is the render thread's, because `tagpu_gaf.c` decodes
  through static buffers.
- **No new file names engine memory**: `tagpu_hpi.c`, `tagpu_vfs.c`, `tagpu_tdf.c`,
  `tagpu_wikiasset.c`, `tagpu_wikiscene.c` and `tagpu_wikiworld.c` are on no line of
  `thread-split.allow`. Every wiki file stays `presentation`, declares no `SIM_*` state, and the
  recorder carries none of it.

### 9. The mockup: the same 3D stage, and what replaces op-for-op parity

- **The pack.** `tools/wiki/gfx.py` is rewritten to resolve each stage description and the
  entries' units by section 5's rules (loose first, then the archives in the engine's mount order,
  first wins; the archive patterns read from the install's own executable, as a mod patches them)
  and to write a tascene-format pack per stage into the gitignored `tools/wiki/mockup/gfx/<entry>/`:
  terrain atlas and tile map, feature atlas and definitions, unit meshes with their slant casters,
  the unit atlas, palette and SHD table, and the resolution report with the same fallbacks.
- **The renderer.** `tools/tascene-view.html`'s parity lane (tagpu's own shaders, extracted from
  the C sources) moves into `tools/tascene-render.js`, which the lab page and the wiki mockup both
  load. The extraction is checked by the lab's own shot md5s, unchanged. `tools/wiki/mockup/gfx.js`
  renders the stage per frame into an offscreen WebGL canvas at zoom 1 × `ss`, with the nano
  staging (a JS port of `tagpu_r3d_nano_state`), the ghost stage and the fog grid of section 7.
  `stage.js` draws R0, the image through the fit's rect with bilinear, and R2 (or R1).
- **The scene, checked op-for-op.** `tools/wiki/mockup/scene.js` turns a view into the scene's
  inputs: tile instances, feature quads with their depth keys, unit and ghost records (model,
  anchor, depth row, pose matrices, alpha, nano, shadow kind). `tagpu/ddraw/src/tagpu_wikiscene.c`
  is its port, pure, compiled natively by `tools/wiki/scenecheck.mjs`, which pairs the two on every
  demo at every 30th tick, exactly or within 1/64 of a unit. This is the 3D stage's `stagecheck`:
  it catches a wrong placement without either renderer. `stagecheck.mjs` itself keeps pairing every
  op of R0, R1 and R2.
- **The image, checked against the game.** `tools/wiki/stage3dcheck.py` compares the game's stage
  image with the mockup's:
  - the game's side is the wiki target before the Gamma curve, written by the trigger
    `tagpu_wiki.shot` (entry, demo, tick) through the wiki instance of `tagpu_vk_world_shot`;
  - the mockup's side is the same frame, rendered headless at the same size;
  - each pixel is classed by the layer the mockup drew last there (an ID pass).

  **Recommended tolerance** (question 8), each a ceiling:
  - **terrain-only pixels**: exactly equal;
  - **feature pixels**: at most 0.05 % differing. tascene's A/B against the game found 52 of
    354 744 terrain-classified pixels different, all on feature-sprite edges, and tree bodies
    identical;
  - **unit and ghost pixels**: a mean absolute difference of at most 2/255, and at most 0.5 %
    differing by more than 40 on any channel. This allows for the two rasterisers' edge coverage
    at `ss` 2; inside tascene alone, `ss` 2 against 1 moves 12 883 unit pixels.

  The first runs measure the floor, and the note records it.

## The landings

The boundaries: what can be checked offline lands first and in parallel; the DLL's visible change
comes in two steps, the ground and then the units. Every landing ends with the documentation pass
and `research/build_wiki.py`, which are not repeated below. "Gate" is `tools/replay/tareplay.py
landing --check` before the commit, and the full `tareplay.py landing` by the runner when it names
groups.

### Landing 6a — the heading, in both simulations

**What changes**: nothing on screen yet; every view and the state hash carry each unit's facing.

**Files:**
- `tools/wiki/mockup/sim.js`: `hx`, `hz` on every unit, set by `walk`, by the work rule and by
  `facing` in a scene; `view()` carries them.
- `tagpu/ddraw/src/tagpu_wikisim.c`/`.h`: the same, function for function; `WIKIVIEW` carries them;
  `tagpu_wikisim_hash` hashes them after each unit's position, in the order the header states.
- `tagpu/ddraw/src/tagpu_wikidata.c`: `facing` read and normalised, refused when zero or not two
  numbers.
- `tools/wiki/simparity.mjs`: the hash twin; `tools/wiki/simcheck.mjs`: rule cases.

**Invariant**: the facing is computed only with ÷ and `sqrt` of values both simulations already
hold, so parity holds by the contract. **Comment that must stand**, above `walk` in both files: why
a vector and not an angle.

**Verification**: `node tools/wiki/simparity.mjs` natively and `--wine`, every demo and rule case
equal (about 1 minute); `node tools/wiki/simcheck.mjs` with four new cases: a step faces its
direction; a builder in range faces its target; a building faces (0, 1); a scene's facing is
normalised; `make -C tagpu/ddraw -j$(nproc)`.

**Gate**: none expected (`tagpu_wikisim.c` is no fixture's source); `--check` confirms.
**Review: medium.** **Thread**: the simulation stays the game thread's.

### Landing 6b — the stage description and its authoring tool

**What changes**: `art/wiki/stage/quickbuild.json` and `buildshapes.json`, and the tool that writes
and checks them. Not packed yet.

**Files:**
- `tools/wiki/stagemap.py` (new): `tiles`, `build`, `check` (section 4); the Python resolver of
  section 5, shared with 6d's `gfx.py` as one module `tools/wiki/tavfs.py`.
- `art/wiki/stage/*.json` (new), written by `stagemap.py build`.
- `tools/wiki/gen-quickbuild.mjs`: the metal-ground strip `[33, 0, 7, 24]` and the black fog rect
  `[0, 0, 8, 7]` move to whole tiles (question 5), then `art/wiki/entries/quickbuild.json` is
  regenerated.
- `research/notes/ingame-wiki.md`: the format, in *The data format*.

**Verification**: `stagemap.py check` exits 0 for both entries (under 1 minute);
`simcheck.mjs`, `simparity.mjs` and `stagecheck.mjs` pass on the regenerated entry; the new files
hold indices, names, counts and hashes only, and the pre-commit hook's provenance check passes.

**Gate**: none expected. **Review**: none (data and a tool; the regenerated demo data is held by
the parity checks).

### Landing 6c — Create()'s hides, one function for the ghost and the wiki

**What changes**: nothing visible. `create_hides` and `create_mask` move out of
`tagpu_datakeys.c` into `tagpu/ddraw/src/tagpu_cobscan.c`/`.h`, pure (a COB blob and its size, a
node walk; bits out), and the ghost's mask calls them there. This is landing 1b's shape: one rule,
two callers, so the wiki's buildings hide exactly what the game's ghost and finished building hide.

**Invariant**: the scan reads only its arguments, every read bounded by the blob's size; the
comment says the ghost's mask and the wiki's models both read what it leaves.

**Verification**: a native `tagpu/ddraw/tests/cobscantest.c` over every stock unit's script on the
local install, against `tools/ta3do` `hidden_at_create`: the same pieces for every unit (about 1
minute); `make`.

**Gate**: `tagpu_datakeys.c` is a `sim` file, and `keys_ghost_mask` names `battle` and
`resource-core-upgrade`; the runner's `tareplay.py landing`. **Review: high** (a sim file).

### Landing 6d — the mockup's 3D stage

**What the owner sees**: `gfx=game` shows the stage as the game will draw it: terrain from the
description's tiles, deposits and vents, units in 3D facing their way, construction in the nano
frame, translucent ghosts, the engine's fog, with order lines, squares, selection and the cursor
over it. The sprites and their baker are deleted.

**Files** (all tools and mockup; no DLL):
- `tools/tascene-render.js` (new) and `tools/tascene-view.html` (now loads it);
- `tools/wiki/gfx.py` (rewritten as section 9's pack builder), `tools/wiki/tavfs.py` (6b's
  resolver);
- `tools/wiki/mockup/scene.js` (new), `gfx.js` (rewritten), `stage.js` (the R0/R1/R2 split and
  the completeness rule), `wiki.js` (the hook);
- `tools/wiki/stagecheck.mjs` (R0 and R2 paired as well as R1);
- `research/notes/ingame-wiki.md`: *In game: what each element is drawn with* rewritten for the
  3D stage; its *Gaps* for `gfx=game` replaced.

**Verification**: `tools/tascene shot` on the lab's fixtures, md5s unchanged (the extraction,
about 2 minutes); `node tools/wiki/stagecheck.mjs`; headless captures of Quick Build demos 1, 3, 8
and 11 and Build Lines demos 2 and 4 at 1920 × 1080 for the owner; a run with `gfx/` absent shows
the whitebox and the strip's notice; a run with one tile hash altered by hand in a scratch copy of a
description shows the whole ground fall back.

**Gate**: none. **Review**: none (no engine code). It can run beside 6e and 6f.

### Landing 6e — the archive reader and the 3D asset load

**What changes**: nothing drawn. At the wiki's first open the DLL resolves both stage descriptions
and every entry's units from the player's archives into the arena, and says what it found.

**Files:**
- `tagpu/ddraw/src/tagpu_hpi.c`/`.h` (new): HAPI archives — header, key, directory, SQSH chunks
  (LZ77 ported from `tools/ta3do` `lz77_decompress`, zlib through `lodepng_zlib_decompress`);
  read-only handles shared for read and write; every bound of section 5.
- `src/tagpu_vfs.c`/`.h` (new): the engine's precedence over the copied list and directory — open
  a path, enumerate a directory pattern — with the duplicate rules batch A establishes.
- `src/tagpu_tdf.c`/`.h` (new): TDF and FBI sections and keys, bounded by length. The DLL's JSON
  reader is not a TDF reader.
- `src/tagpu_wikiasset.c`/`.h` (new): the 3D loader thread; the descriptions, the TNT tiles and
  their hashes, the feature sections and their frames or models, the units' FBI, 3DO, texture
  frames and Create() masks (6c's `tagpu_cobscan.c`); the arena and its report; one interlocked
  publish (`tagpu_wiki3d_get()`); the trigger `tagpu_wiki3d.dump` → `tagpu_wiki3d.json` (each
  element: its path, the archive it came from, resolved or the refusal, faces and bound textures).
- `src/tagpu_replays.c`/`.h`: `int tagpu_replays_archive_paths(char (*out)[0x104], int max)`, the
  mount list's paths in mount order on the game thread, next to `running_mod`, which reads the same
  list (question 2).
- `src/tagpu_wiki.c`: the first open copies the list and `GetCurrentDirectoryA` and starts the
  loader.
- `tagpu/ddraw/tests/wikiassettest.c` (new): the same resolution compiled natively against the
  local install.

**Invariants** (comments at each): the arena is immutable once published and never freed; it lies
inside the readers' range, which is checked once at its allocation; every offset in a file is data
until bounded against that file; the loader reads no engine memory and paints nothing.

**Verification:**
- `wikiassettest` built with gcc, AddressSanitizer and UndefinedBehaviorSanitizer, its JSON equal
  to `gfx.py --resolve-json` on the same install: the same archive for every path, the same tile
  hashes, the same face counts and texture bindings (about 1 minute).
- A fuzz pass: 1 000 copies of one archive and one 3DO with bytes truncated or flipped, every
  refusal named, no sanitizer error (about 3 minutes).
- One game launch, `tools/wiki_regression.py <instance> asset --launch` (about 4 minutes): open
  the wiki, wait for `tagpu_wiki3d.json`, compare it with the native run's; the log's load time and
  arena size.

**Gate**: `tagpu_replays.c` is a source of the `resource-*` groups; the runner's `tareplay.py
landing`. **Review: high** (untrusted file parsing, a new thread, an allow-list argument grown).
**Thread**: the loader thread reads files; the copy is the game thread's.

### Landing 6f — the per-scene modules instanced

**What changes**: nothing drawn. The wiki's instances of the seven files exist, link, and are
proven isolated; the game's are byte for byte what they were.

**Files:**
- `src/tagpu_scene.h` (new): the four duties of section 1.
- The seven files: the include and the header paragraph; `tagpu_posebake.c`'s `PB_SCRATCH`.
- `tagpu/ddraw/Makefile`: each of the seven compiled again as `<name>.wiki.o` with
  `-DTAGPU_SCENE_WIKI`, linked into the DLL.
- `tools/scene-check.sh` (new): the undefined-symbol allow list, an order-only prerequisite of the
  link.
- `src/tagpu_wikiworld.c`/`.h` (new): the stubs and the redirected sources, each answering
  "nothing this frame".
- `research/notes/gpu-status.md`: the hook map's note that the world passes have two instances.

**Invariants**: the two of section 1, each stated in `tagpu_scene.h` and in the check's header.

**Verification:**
- `make -C tagpu/ddraw -j$(nproc)` with `scene-check` passing.
- A planted call from a wiki object into `tagpu_vk_shadow_prepare` fails the link, naming it.
- `size` of the seven `.wiki.o`, recorded.
- The world's A/B captures on `scenarios/tascene-parity.json`, paused at a fixed GameTime (the
  terrain, feature and unit levers and the world shot), byte-identical between the previous DLL and
  this one: one launch, about 5 minutes.

**Gate**: none expected (no fixture names these files); `--check` confirms. **Review: high** (it
touches every world pass file and the build). It can run beside 6e.

**Status (2026-10-26): built and verified on its branch, not reviewed or landed.** Every
verification above ran:
- `make -C tagpu/ddraw -j$(nproc)`, `LIMITS=stock` and `COVER=1`: clean, `scene-check: clean -- 7
  wiki object(s), 175 undefined symbol(s)`; thread-split, sim-state, sim-features and spirv clean,
  and no shader header needed regenerating.
- The planted call: `scene-check: src/tagpu_vk_world.wiki.o calls tagpu_vk_shadow_prepare, which a
  module of the game's world defines`; a planted unrenamed function fails the link with `multiple
  definition`.
- The game's objects: six of the seven byte-identical to the base commit's; `tagpu_posebake.o`
  differs because its scratch has external linkage (`.bss` offsets and one register choice).
- Sizes: `gpu-status.md` §2.12d.
- The A/B on `tascene-parity`, the base commit's DLL against this one, each launched on a private
  Xvfb and paused (`keys pause`, the tick read twice): the terrain, feature and unit captures
  (`tacli ab terr|feat|posedraw`, each pass armed alone) are byte-identical, md5 for md5; the
  presented window under the whole play set (seven passes in the census, 1 966 colours) differs by
  0 px below the message band, the cursor's pulsing hotspot pixel aside, which also differs
  between two grabs of one run. The band's 2 902 px are the "forces obliterated" lines, whose
  wording is random per boot (seen in a crop of both).
- `tareplay.py landing --check`: pass, the note on `848309b7` covering every gated file; no group.

What 6f did not settle and 6g takes: the seam's declarations of the wiki's entry points (section 1
says why the header cannot simply be included again), and the wiki instances' device memory.

### Landing 6g — the ground in 3D

**What the player sees**: the stage's ground in the game's own tiles, with its deposits and vents
and the engine's fog; units still as whitebox boxes, now over the 3D ground (R2).

**Files:**
- `src/tagpu_wikiscene.c`/`.h` (new, pure): the view and the arena's metadata → tile instances,
  feature quads and their depth bands (flat features below the units, tall ones in their row, as
  `tagpu_feat.c` bands them; the arithmetic carries a pointer to that file), the fog grid.
- `src/tagpu_wikiworld.c`: the terrain hand-over (its R8 atlas is the arena's, built by the
  loader); the feature hand-over and its `TAGPU_GAFATLAS`, painted on the render thread with no
  allowance; the target geometry (section 2); the completeness facts the seam reads.
- `src/tagpu_overlay.c`: `tagpu_wikiworld_frame(f)` after `tagpu_wikistate_present(f)`.
- `src/tagpu_vk.c`:
  - the wiki instances' prepare in the data order the game's follow, all before the HUD pass's
    prepare, so the choice between the ranges rests on this frame's answers;
  - the wiki world render pass after the game's and before the main one;
  - the wiki composite between R0 and R2;
  - their teardown handshakes (`_down`, `_down_owed`, `_down_paid`) beside the game's.
- `src/tagpu_vk_hud.c`/`.h`: the wiki layer recorded as its three ranges, chosen at record time.
- `src/tagpu_wikistage.c`/`.h` and `src/tagpu_wikistate.c`: R0, R1 and R2 built every frame.
  `WIKISTAGEART` gains the per-type resolution bits and the ground's state.
- `src/tagpu_wikipk.h`, `src/tagpu_wiki.c`: the record's new fields (section 6) and their copy.
- `tools/wiki/scenecheck.mjs` with `tagpu/ddraw/tests/wikiscenetest.c` (new);
  `tools/wiki/stage3dcheck.py` (new).
- `tools/hudpak.py`: `stage/*.json` into `WIKIPAK`; `src/tagpu_wikidata.c` keeps the descriptions
  for the loader.

**Invariants and the comments that must stand:**
- `tagpu_wikiworld.c`: the hand-overs live for this render-loop iteration and are stamp-checked;
  the atlas is painted on the render thread only.
- `tagpu_vk.c`, at the choice: the image is drawn whole or the whitebox is, decided from this
  frame's prepares.
- `tagpu_wikiscene.c`: one fit for the composite and the ops.

**Verification:**
- `node tools/wiki/scenecheck.mjs` and `stagecheck.mjs` (each under 2 minutes).
- One game run, `wiki_regression.py <instance> stage3d --launch` (about 6 minutes):
  - Quick Build demos 1 and 11 and Build Lines demo 1, each `tagpu_wiki.shot` at three ticks;
  - `stage3dcheck.py` against the mockup, at `ss` 1 and 2: terrain pixels equal, feature pixels
    within question 8's ceiling;
  - the levers `tagpu_wiki3d.on` `nomap` and `nofeat` show R1 whole;
  - captures at 1920 × 1080 and 3840 × 2160 for the owner;
  - the wiki scene's render-thread time logged.

**Gate**: `tagpu_wikipk.h` reaches three sim files through `tagpu_packet.h`, so 34 groups; the
runner's `tareplay.py landing` (14 minutes at landing 2: risk 4). **Review: high** (a render pass
and a composite added to the frame, per-slot resources, the record's growth).

### Landing 6h — units in 3D

**What the player sees**: every unit as its model, facing where it walks or works, buildings
rising in the nano frame, translucent ghosts at the cursor and the gesture's sites, shadows as
the game's options give them; a type the running game does not have stays a box.

**Files:**
- `src/tagpu_wikiscene.c`: the model records of section 6 (pose matrices, depth rows, nano, shadow
  kind, alpha).
- `src/tagpu_wikiworld.c`: through the wiki posebake, render3do and posedraw instances (their
  `_frame` calls in their order, level generation constant, the atlas's allowance off at the first
  frame); the completeness count of baked records; 3D features through the same path.
- `tools/wiki/mockup/scene.js`, `gfx.js`: the same, if 6d left any of it out.

**Invariants**: every node pointer handed to the wiki bake lies inside the arena, which the
producer checks before the call (a bound on a value, not a probe); the wiki bake sees only the
arena.

**Verification:**
- `scenecheck.mjs` with the unit records.
- One game run, `wiki_regression.py <instance> units --launch` (about 7 minutes):
  - Quick Build 3 and 8 (construction, a reclaim) and Build Lines 2 and 4 (ghosts), shot at fixed
    ticks;
  - `stage3dcheck.py` within question 8's tolerance;
  - a unit walking toward +x captured facing +x;
  - a reclaim captured, its nano staging compared with the game's own on a real reclaim
    [INFERRED that the engine stages a reclaim as a build run backward];
  - the lever `nounit=ARMCK` boxes ARMCK alone;
  - the wiki scene's render-thread time against 0.5 ms.
- `wiki_regression.py mp` with a demo playing (about 8 minutes): both peers' logs desync-free; the
  render-thread time on the host.

**Gate**: the 34 groups, as 6g. **Review: high.**

### Landing 6i — the 3D stage under the mods

**What changes**: what each supported setup shows, measured and written down; code only if a run
finds a bug.

**Verification**: one scripted run per setup through the compatibility suite's launcher
(`tools/compat/tacompat.py`'s setups), about 6 minutes each, one at a time under the run lock:
retail, Escalation, Total Mayhem, ProTA, TA Zero and Twilight. Each run: a skirmish, the wiki opened
on both entries, `tagpu_wiki3d.json`, two captures, the display polled for error boxes. The note
gets the table: per setup, which archive each unit and the ground came from and what fell back.

**Gate**: none unless code changes. **Review**: none for docs.

## What runs in parallel

| Track | Touches | Needs |
|---|---|---|
| 6a, the heading | `sim.js`, `tagpu_wikisim.c`, `tagpu_wikidata.c` | nothing |
| 6b, the description | `tools/wiki/stagemap.py`, `tavfs.py`, `art/wiki/stage/`, `gen-quickbuild.mjs` | nothing |
| 6c, the shared scan | `tagpu_datakeys.c`, `tagpu_cobscan.c` | nothing |
| 6d, the mockup | `tools/tascene*`, `tools/wiki/gfx.py`, the mockup | 6b's format |
| 6e, the reader | `tagpu_hpi.c`, `tagpu_vfs.c`, `tagpu_tdf.c`, `tagpu_wikiasset.c`, `tagpu_replays.c` | 6b's format, 6c |
| 6f, the instances | `tagpu_scene.h`, the seven files, `Makefile` | nothing |

6g needs 6d, 6e, 6f and landing 4 integrated; 6h needs 6g and 6a; 6i needs 6h. The merge surfaces
are `tagpu_wikipk.h`, `tagpu_wiki.c` and `tagpu_wikistage.c`, which landing 5 (Try it yourself)
also extends: integrate in order. QUESTIONS batches A to D are 6e's inputs and do not block 6a to
6d or 6f.

## Risks

1. **The instancing is unusual.** A reader of a pass file may not expect it to be compiled twice.
   The header paragraph, `tagpu_scene.h` and `scene-check.sh` say so where they would look, and the
   link fails on the two mistakes it invites (a missed rename, a call into a game-only module).
2. **The seam grows a render pass and a composite per frame while the wiki is open**, and more
   teardown handshakes. The A/B identity of 6f covers the game's path; 6g's run measures the wiki
   scene's cost.
3. **Untrusted archives.** A malformed or hostile archive is parsed on the loader thread; the
   bounds of section 5 and 6e's fuzz pass are the defence. A crash there would take the game down.
4. **The replay gate is longer than ten minutes** (34 groups, 14 minutes at landing 2) for every
   landing that touches `tagpu_wikipk.h`. Scoping it down would be a loosening; batch 6g and 6h if
   the gate's length matters.
5. **The first open in play uploads the wiki atlases through the banded path**, which waits on its
   fences (`tagpu_vk_stage.h`): a stall of some milliseconds once, to be measured in 6g.
6. **The arena's size** under a mod with large textures; read frames, not whole GAFs, and log the
   size (6e).
7. **The yaw's lateral sign and a reclaim's look are [INFERRED]**; each is a capture in 6h.
8. **WebGL and Vulkan rasterise unit edges differently at `ss` 2**; hence a tolerance for unit
   pixels, the owner's to set (question 8).
9. **A mod's model larger than the stock footprint the simulation uses** overhangs its box; the
   demos' figures are the stock game's, as the wiki's note already states.
10. **Landing 4's integration is not on this checkout**; 6g takes its names as they land.

## Questions for the owner

**Answered 2026-10-26: the owner took every recommendation.** The loosenings of questions 2 and 3
are approved (`tagpu_replays.c`'s argument grows by the mount-list copy; `tagpu_posebake.c`'s and
`tagpu_render3do.c`'s arguments are reworded to cover their wiki copy), the tolerances of
question 8 are accepted, and questions 1, 4 to 7 and 9 to 11 proceed on their recommendations.

Each with the recommended answer. Questions 2, 3 and 8 are loosenings in CLAUDE.md's sense; the
rest are design choices the specification leaves open. None is an escalation: every verification
above can run, and no fix depends on timing.

1. **How the second scene is made.** *Recommended: compile the seven per-scene modules a second
   time* (section 1). Isolation holds by C's linkage for today's statics and tomorrow's, and the
   game's path does not change. The alternative, a per-scene struct in each file, changes every
   line the game draws through and leaves isolation to each later edit.
2. **The mount list's copy: a loosening.** `tagpu_replays.c`'s `thread-split.allow` argument grows
   by "and copies the mount list's paths, on the game thread, for the wiki's asset loader".
   *Recommended: yes.* It reads the same list already (`running_mod`), so no new engine address is
   named; the alternative is a new `publisher` line for `tagpu_wiki.c`.
3. **The arguments of `tagpu_posebake.c` and `tagpu_render3do.c`.** Their `fenced` lines would
   also say that their wiki instance reads only the wiki's arena, immutable and never freed. No
   new engine memory is read, but an allow-list argument changes. *Recommended: yes.*
4. **A hash per referenced tile in the committed description** (FNV-1a 32 of the tile's bytes,
   and each source's tile count): numbers derived from the game's data, beyond indices and names.
   *Recommended: yes.* They cannot rebuild a pixel, and they are what makes a same-named map with
   other tiles fall back instead of drawing a patchwork.
5. **The scenes' tile-sized rects.** Quick Build's metal-ground strip `[33, 0, 7, 24]` and its black
   fog `[0, 0, 8, 7]` move to whole tiles (even cells), so the ground's tiles and the engine's fog
   match the rules exactly. *Recommended: yes*; the demos' claims are unchanged (`simcheck.mjs`).
6. **Fog in the engine's style** on terrain and features (feathered corner masks), units never
   fogged. *Recommended: yes*, rather than the whitebox's translucent cells over the 3D ground.
7. **The stage's look is Classic in landing 6**: palette colour and the engine's shade rows,
   whatever Classic++ setting the player uses. Classic++ (the restored twins, the lighting) is a
   later landing, since the restorer's job pool is one of the game-only modules section 1 stubs.
   *Recommended: yes.*
8. **The image tolerance: a loosening.** Terrain pixels equal; feature pixels at most 0.05 %
   differing; unit and ghost pixels within a mean of 2/255 and 0.5 % past 40 (section 9).
   *Recommended: approve these as the ceilings; the first runs' measured floor is written in the
   note.*
9. **Builders face their work.** A builder in range turns to what it builds or reclaims; units turn
   instantly, with no turn rate. *Recommended: yes*: it reads as the game does at the stage's
   scale, and it costs one rule in both simulations.
10. **The ground falls back whole** if any tile or deposit fails, rather than element by element.
    *Recommended: yes*, so the picture never shows a rule without its art or art without its rule.
11. **The stage's cursors stay the kit's icons** in landing 6, as specified, rather than TA's
    `cursors.gaf`. *Recommended: yes*; TA's own is a later choice, made in the mockup first.

## What this plan does not know yet

- **QUESTIONS batches A to D**:
  - how the engine resolves duplicate paths and duplicate `UnitName`s;
  - the model and script paths;
  - the texture binding rule and the texture bank's order;
  - the TNT header the loader reads;
  - a 3D feature's logo colour;
  - whether any mod moves an asset directory.

  6e implements the answers; none changes the design.
- **Whether the in-memory 3DO and GAF are the files rebased in place** (A4, A6). If the loader
  reshapes them, the arena builds the engine's shape instead; the readers still do not change.
- **The wiki instances' sizes after resizing**, and the first open's load time and stall.
- **The image diff's floor** for unit pixels.
- **The geothermal plume** (feature animation or particles) is still not established; the stage
  shows the vent's frame as the engine's feature pass draws it.
