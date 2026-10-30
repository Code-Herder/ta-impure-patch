# Palette, Gamma and the full-colour pipeline

Where the palette and the Gamma reach the world, and Classic on the full-colour pipeline.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.3f The palette, and the one place the Gamma reaches the world (`tagpu_pal.c`, `tagpu_vk_world.c`) — G20c

`main+0x143A7` is the engine's own palette table and **it is not what the screen shows.** Every
palette the engine sets goes through `0x4BA200`, which keeps the entries in the graphics globals
and hands DirectDraw `min(255, trunc(entry × *(float*)(globals+0x614)))` — the Gamma factor
(`SetGamma 0x4BA590`; an option screen computes `0.5 + Gamma/24`, `+gamma N` in chat sets `N/10`
outright) — and never scales `+0x143A7` ([engine map](../exe-reverse-engineering.html), "The palette
the screen is presented with"). So a world drawn from `+0x143A7` alone is the wrong brightness
beside the engine's own pixels at any factor but 1.0, and the question is where the factor goes.
**The world takes it once, on the finished image; the UI takes the presented table.**

`tagpu_pal.c` holds both tables, and each has one kind of consumer:

- **The engine's table, `tagpu_pal_engine()`, is every world colour's source.** The publisher
  copies `+0x143A7` into every packet on the game thread (1 KB, with the factor beside it), and
  `tagpu_pal_frame` keeps the copy and an **engine serial** (`tagpu_pal_engine_serial`) that
  moves only when the bytes do, logging `pal: the engine's table changed (engine serial=N)` for
  each of the first four changes and at most once a second after that. Built from it: the RGBA8
  **base atlases** every world pass samples — terrain, features, effects, units, through
  `tagpu_pal_expand` (§2.88) — which are also the Classic++ restorer's input, the markers' `uPal`,
  the effects' LHT flash colours, and the units' face-shade multipliers (`shade_k_build`, §2.88).
  The table has **one writer**, the PALETTE load `0x42A400` that `UIPipelinesInit` runs once per
  process (engine map, "Who writes the engine's own table"), so the engine serial is 1 for the
  life of an ordinary process — MEASURED on a `big-battle` soak, one line per process — and no
  world source is rebuilt in play. **Every world source keys on the serial**, so an engine writer
  the survey missed would rebuild them rather than leave them stale: the base atlases (`s_bPal`
  in the feature, effect and unit passes' `base_upload`, `s_basePal` on the terrain), the
  restore jobs (`s_rjPal` in each world lane: a move is a new job, and the twin is blanked and
  repainted from the re-sent base), `k[]` (`s_shadePal` in `tagpu_r3d_shade_want`, which also
  keeps the last shade table it saw) and the LHT flash colours (`s_lhtPal`, with the LHT bytes).
  The markers upload `uPal` every frame. MEASURED 2026-09-24 with a diagnostic-only forced bump
  of the serial mid-battle (§2.88): every one of them rebuilt on the frame it was seen.
- **The presented table, `tagpu_pal_live()`, is the UI's.** It is resolved lazily: the per-present
  `tagpu_pal_frame` marks it stale and the first reader re-resolves it — the primary's palette
  object (`g_ddraw.primary->palette->data_rgb`, what cnc-ddraw's `ddp_SetEntries` stored) read
  under the fork's `g_ddraw.cs`, the engine's table the fallback until a primary exists — into a
  1024-byte snapshot of ours. **The lifetime is the fork's lock, not a probe**: the game thread
  NULLs `g_ddraw.primary` inside that section and frees the object only after leaving it, so a
  pointer read *and dereferenced* inside it is a live object or NULL, never a freed one
  (`CLAUDE.md`, *Fixes must be safe by construction*). The shell's fades and per-screen palettes
  are presented-palette events and the UI draws the shell, which is why the UI keeps this table
  and the one repaint path left: when `tagpu_pal_serial()` moves, `tagpu_gui_surf.c` calls
  `tagpu_gaf_atlas_restore_repalette` and the UI's twin re-restores without clearing
  (`restoreRepaint`/`restoreBlanks` in its hand-over, §2.51). No world lane has either field.

**The Gamma is applied once, by the world composite.** `tagpu_vk_world.c` draws the finished
world target onto the swapchain between TA's own frame and the UI. When `tagpu_pal_gamma()` is
1.0 it draws with `tagpu_native.c`'s plain `DFS`, so the stock Gamma is the picture with no curve
at all; otherwise with `GFS`, which samples `uGam` (binding 41), **the engine's own curve**: a
256 × 1 R8 image whose texel `e` is `min(255, trunc(e × factor))`, what `0x4BA200` hands
DirectDraw for an entry of `e`. It is built in double on the CPU, because the engine truncates an
exact x87 product and a float product can land on the integer above it. A level between two (the
supersample box filter's average at an edge) takes the line between their outputs. The target is
premultiplied, so the curve is applied to the un-premultiplied colour and the coverage put back;
a texel with no coverage is additive light (the effects' flashes) and takes the curve as it is.

- **One curve per slot, for the target's own reason**: the composite of frame N samples slot N's
  curve while frame N+1 may be writing one. Slot `i`'s is rewritten only in slot `i`'s `prepare`,
  behind the seam's fence wait on that slot and a write-after-read barrier, and only when the
  factor it holds is not this frame's — a Gamma change reaches the screen on the frame that
  carries it, and a steady factor costs nothing.
- **The world A/B capture reads the target, before the curve**, so a world A/B compares the
  unscaled world at any Gamma, and a capture no longer depends on the instance's Gamma.
- **Nothing in the world repaints when the Gamma moves**: the curve is the whole response. The
  restored twins, the base atlases and every LUT are functions of the unscaled table, so the
  restorer's tileability threshold (`tagpu_rglsl_tileable`, a raw colour distance) classifies the
  ART and not the display without a special case.
- **A frame with no world target takes the factor in the blend unit**
    (`tagpu_vk_world_record_direct`, `tagpu_native.c` `KFS`). Such a frame is one where
    `tagpu_vk_world_prepare` returned 0:
    - for good, on an sRGB surface, with no depth format, on a format the device will not both
      render and filter, or on a slot the device will not give the memory for;
    - for that frame alone, on a size past `WORLD_MAXDIM`, or when the gather published no geometry.

    Its world passes draw straight into the swapchain image, so there is no image to sample. One
    full-frame quad, after the last world pass and before the UI, multiplies the frame by the factor:

    - above 1: a source of 1.0 times `DST_COLOR`, plus the frame times `CONSTANT (factor − 1)`,
      which is frame × factor. Blend constants clamp to [0,1], so a factor above 2 doubles first;
      a doubling is exact until it clamps, and a clamped level stays clamped;
    - below 1: `frame × CONSTANT (factor)`;
    - at 1.0: nothing is drawn.

    The factor lands after the world's own blends, and clamps where the curve does, as on the
    composite. **On a UNORM surface** it differs in one step: the blend rounds `e × factor` where
    the engine truncates, so a level lands at most one step from the curve. **On an sRGB surface it
    does not**: the blend runs on decoded, linear values, so the factor scales light rather than
    the level — at factor 1.5, level 128 lands near 155 against the curve's 192. That surface is
    exactly where this path is the only one, because the target refuses sRGB (above). The seam
    prefers `B8G8R8A8_UNORM` and falls back to the device's first format only when that is not
    offered (`tagpu_vk.c`, the surface format), so sRGB is a fallback surface; the choice of format
    is unchanged here and belongs to the owner. The frame holds only the clear and the world when
    the quad draws: nothing of the engine's is drawn under the world, the UI comes after, and the
    default clear is black. Its two pipelines and its quad are its own, so they survive the target's
    refusal; `tagpu_vk_world_down` frees them with the device or a resize. **MEASURED 2026-09-24**
    (§2.88): against the target path at `ss=1`, 69.6 % of the world's channels exact at Gamma 16 and
    75.2 % at Gamma 6, the rest off by 1; byte-identical at Gamma 12.
- `tagpu_order.c`'s `seq_ink` reads the packet's `pal[]` directly, the same unscaled table, from
  the packet the order walk already holds; its question is a luminance ranking over one sprite's
  colours, which no uniform scale moves.

**Everything in `tagpu_pal.h` is render-thread only**: its entry points resolve into shared
snapshots, so a call from the game thread would race them. The heartbeat's `palchg=`,
`paldiff=n@i` and `palsrc=` describe the presented table and are recomputed only when a reader
resolves it and the bytes changed — with the UI layer off nothing resolves it, and `paldiff` is
the value from the last change, not a live comparison. The log line `pal: presented palette
changed (serial= src= diff= gamma=)` is rate-limited to one a second, so a campaign fade cannot
flood it.

**MEASURED 2026-09-23** (G20c, Two Continents, 1024×768, `ss=2`):

- **The factor moved live, and the whole world followed with nothing restored.** Classic++ at
  the stock Gamma (factor 1.0), then `+gamma 15` typed in chat on the running game: the
  presented window's 880 × 688 world rectangle at 1.5, against the same rectangle at 1.0 put
  through the engine's curve, is exact on **580 180 of 605 440** pixels (95.8 %) and within ±1 on
  all but one — the pixel under the cursor. The ±1 is the curve applied to a box-filtered
  average rather than to each sample. The log shows no restore after the first and no second
  `pal: the engine's table changed` line.
- **Against the previous design, which scaled the palette before the shading** (2b's world
  target through the curve, against 2c's, per pass and preset, at Gamma 0, 6, 16 and 20 —
  factors 0.5, 0.75, 1.167 and 1.333; `los 1`):
  - **Classic terrain** is exact at 0.75 and ±1 elsewhere (a product truncated once instead of
    twice), except in the grey fog band above 1.0 — 68 668 px at 1.167, 69 200 at 1.333, by up
    to 29 — where the old grey was the mean of channels the scale had already clipped.
  - **Classic units** differ by ±1 or at a channel that clips on at most 1 200 of ~1 540 drawn
    pixels, and above 1.0 on a few more (16 px at 1.167, max 34; 1 at 1.333): faces whose
    unscaled colour is near white, which 2b clipped at 255 **before** shading. The engine remaps
    in index space and scales on presentation, which is 2c's order, not 2b's.
  - **Classic features** differ by ±1, at a clip or in the grey band.
  - **Classic++ differs by design, up to 63 on terrain at 1.333**: 2b's restorer was fed the
    Gamma-brightened art and its lambert multiplied the presented colour; now the restorer sees
    the art at the brightness it was trained on and the lambert multiplies before the curve.
- **At Gamma 12 (factor 1.0), the stock value, the curve is not drawn**, and every 2c capture is
  byte-identical to 2b's: terrain, units and features, with and without fog, both presets.

### 2.88 Classic on the full-colour pipeline, the Gamma once, the 8bpp path deleted — G20c and G20d

**What it is.** The owner's calls of 2026-09-23 ([BAR camera & full-colour Classic](../bar-camera-port.html)
Part 2): Classic is a **preset** of the Classic++ pipeline, `assets=0 light=0`, in full colour,
with no fidelity kept to the 8bpp colours; units take an RGB multiplier per `PALETTE.SHD` row in
both presets; the fog's grey is the RGB mean everywhere, markers included; the Gamma is applied
once, to the finished world; the UI stays indexed and GDI is untouched. Four steps, one commit
each: 2a and 2b are G20c, 2c and 2d are G20d. `tagpu_classicpp.on`/`.off` remain as the levers
that pick the preset; nothing deleted keeps an alias.

**What every world pass samples now — the base atlas.** Each world atlas has an **RGBA8 base**
beside its index mirror, and it is the only atlas image the world passes upload:

| atlas | base | built by | alpha |
|---|---|---|---|
| terrain | `atlasW × atlasH` (2176 × 2720 on Two Continents) | `tagpu_vk_terr.c`, the tile atlas through `tagpu_pal_expand` | 255 everywhere (a tile has no hole) |
| features | 2048² | `tagpu_vk_feat.c` `base_upload` | 0 at the frame's key, else 255 |
| effects | 2048² | `tagpu_vk_fx.c` `base_upload` | 0 at the key, else the flash level (below) |
| units | 2048² | `tagpu_vk_unit.c` `base_upload` | 0 at the key, else 255 |

- **The colour is the engine's unscaled table** (`tagpu_pal_engine()`, §2.3f), and the base is
  keyed on the atlas's mirror serial and the engine serial: a paint re-sends only the tiles the
  atlas's dirty map says changed (`tagpu_gaf_dirty_since`), an engine-table change the whole
  page. The staging is the slot's own bounded buffer, at most 2 MiB (`TAGPU_VK_STAGE_CAP`)
  whatever the atlas (`tagpu_vk_stage.c`, *The staging* below); the image is shared and written behind a
  write-after-read barrier on the one queue.
- **The key is per frame, so it has a plane of its own.** `tagpu_gaf.c` writes `keym` beside the
  index mirror in the same paint and the same allocation, 0 where a texel is its frame's key;
  `tagpu_pal_expand` makes that alpha 0 with RGB 0. Every reader discards at `alpha < 0.5`, which
  is exactly the old per-vertex `index == ck` test.
- **The effects' flash level rides in the alpha.** The flash remaps an index through the LHT by
  `index − 0x4F`; the base stores `TAGPU_FX_FLASH_ALPHA(i) = 255 − clamp(i − 0x4F, 0, 31)`, never
  below 224, so the hole test is unchanged and the shader recovers the level as
  `255 − int(a·255 + 0.5)` — exact.
- **The restorer reads the base** (`srcBase` 1 on the terrain, feature, effect and unit jobs), not
  indices through a palette. The UI's job still reads its R8 through the presented palette.
- **What keeps a shared atlas image alive while frames in flight sample it** differs by pass:
    - **Features and effects write their atlas descriptors once**, on the first prepare after
      `build`, before any set is bound. `base_upload` refuses a change of the atlas's dimension
      and stands the pass down rather than rewrite sets that frames in flight use.
    - **Units write the view into the slot's set every frame** (`bind_main`), so the hazard is
      the image, not the set: re-creating it would free an image another slot's pending submit
      samples. `atlas_build` refuses a change of the square once the image exists and stands the
      pass down, as the other two do.
    - None of these refusals fires today: the three atlases are `ATLAS_DIM`-square, a
      compile-time 2048 in each of `tagpu_render3do.c`, `tagpu_feat.c` and `tagpu_fx.c`. The
      guards keep that true.
    - **The terrain's atlas does change size**, once per map, and the terrain pass rebinds every
      prepare (`shared_bind`, behind that slot's fence). It replaces an image through
      `shared_resize`, one retire at a time: the old image is destroyed only when no slot's set
      still names it (`SHARED::pending`), and a frame that finds a retire still clearing draws
      nothing rather than the previous map's texels.

**The shading, both presets.** Every world fragment shader has one colour path: the restored twin
where `uRestored` is 1 and the twin's alpha says it has painted, else the base; then the preset's
terms; then the grey band. A palette index reaches a shader only as a flat colour — an
untextured face, the nanoframe band ramp, an effect's line, a marker — or the unexplored black,
and is looked up in `uPal`, the same unscaled table.

- **Units: the face-shade multiplier.** The vertex stage computes the row the engine's lit rasteriser
  would (§2.106: `ftol(5 · N·L) & 0x1F` per vertex, row 0 taken as 5, for a structure under the
  Shading option, and the neutral row for everything else); the fragment multiplies RGB by `k[row]` and
  clamps to 1 — not on a nanoframe band, which the engine paints over the shaded index — then the
  lambert (exactly 1.0 under `light=0`). `k` is built once per process by `tagpu_render3do.c`
  `shade_k_build` from the packet's SHD and the engine's table: per row, the least-squares slope
  through the origin of shaded against unshaded colour over every entry and channel whose shaded
  value is below 250, normalised to the neutral row. It reaches the unit pass as a 32 × 1
  `R32_SFLOAT` image per slot (`uShadeK`, binding 47). **Measured on the live table** (neutral
  row 15; `r3d shade:` in `tagpu.log`):

  | rows | k |
  |---|---|
  | 0–7 | 0.000 0.063 0.131 0.196 0.265 0.337 0.402 0.477 |
  | 8–15 | 0.542 0.608 0.679 0.747 0.823 0.877 0.985 1.000 |
  | 16–23 | 1.078 1.139 1.175 1.243 1.304 1.350 1.391 1.434 |
  | 24–31 | 1.488 1.520 1.539 1.603 1.597 1.628 1.698 1.741 |

  With no SHD the computed ramp `0.60 + 0.025·row` stands in (neutral 16).
- **The grey band is `(R+G+B)/3`** (`TAGPU_GLSL_FOG_GREY_RGB`) after the colour is lit and
  shadowed, in every world pass and both presets — never snapped to a palette entry. The engine's
  remap table (`TAProgram+0xCC`) is read by nothing of ours.
- **Shadows follow the key alone.** The unit pass's hard-shadow gate is
  `cppL->shadows == TAGPU_SHADOWS_HARD` in both presets; it used to draw the hard shadow under
  Classic whatever the key said. Every state the Shadows row reaches is unchanged (it writes the
  key and the engine's bits together); a hand-written `shadows=0` under Classic now draws none.

**The Gamma, once** — §2.3f: the world composite applies the engine's own curve through a
256 × 1 R8 image per slot (`uGam`, binding 41 of `tagpu_native::GFS`), and at factor 1.0 draws the
plain resolve. A frame with no world target draws its world straight into the swapchain image,
and takes the factor from one full-frame quad after the last world pass and before the UI. That
quad multiplies the frame by the factor in the blend unit (`tagpu_vk_world_record_direct`), and
it lands within one level of the curve.

**What 2d deleted**, and who was ported:

- the four R8 device atlases, their staging (`astage`, the unit pass's `atlasNeed` reservation)
  and `atlas_upload` in the feature, effect and unit passes (the terrain's R8 image with them);
- the unit pass's per-slot SHD LUT image (256 × 32 R8) and every pass's per-slot fog-shade LUT
  (256 × 1), and the feature pass's per-slot palette (it has no index left to look up);
- `uLit`, `uFogLUT`, `uLUT`, `uAtlas` and `TAGPU_GLSL_FOG_SHADE`, and the Classic branches under
  them in the terrain, feature, unit, effect and marker shaders;
- **the packet's fog-shade field**: `fogsh_off`/`fogsh_len`, `TAGPU_PK_FOGSHADE_BYTES`,
  `TAGPU_PK_TRUNC_FOGSH` and `tagpu_pk_fogshade()`, with its publisher `fogshade_snapshot`;
- `tagpu_native_foglut()`; `tagpu_r3d_lut_want`/`tagpu_r3d_lut_mirror` and the computed
  nearest-colour LUT, replaced by `tagpu_r3d_shade_want` and `tagpu_r3d_shade_k`;
- the hand-over fields `lit`, `fogLut` (terrain, features, effects, units), `lut`/`lutW`/`lutH`/
  `lutSerial` (units), `fogLut`/`index` (markers), and every world lane's
  `restoreRepaint`/`restoreBlanks` (2c);
- `tagpu_classic8.on` and `tagpu_classicpp_index()` — the old Classic, kept behind a lever for
  2b's re-baseline and for nothing after it;
- in the lab, `tools/tascene-view.html`'s index path for its Classic lane: the parity lane samples
  the base atlas and the `k` table, as the game does ([tascene](../tascene-design.html)).

The function inventory of `tagpu/ddraw/src/*.c`, the G20c base against 2d (`78c22b2` →
`402dc94`), counting one-line definitions too: **2060 → 2066**, 12 gone and 18 new. Gone —
`tagpu_native_foglut`, `fogshade_snapshot`, `shade_build_lut`, `shade_upload`,
`tagpu_r3d_lut_mirror`, `tagpu_r3d_lut_want`, `atlas_upload` ×3, the feature pass's
`slot_build`, `slot_drop_astage` ×2. New — `tagpu_pal_engine_serial`, `tagpu_pal_expand`,
`mirror_bytes`, `mirror_wrote`, `tagpu_gaf_band_since`, `shade_build`, `shade_k_build`,
`tagpu_r3d_atlas_key`, `tagpu_r3d_shade_k`, `tagpu_r3d_shade_want`, `base_upload` ×3,
`slot_drop_bstage` ×2, `build_gamma`, `gamma_free`, `gamma_upload`. (An inventory that counts
only a definition whose `{` ends the line or opens the next misses the one-line functions — 80
at the base, 81 after 2d, `tagpu_pal_engine_serial` the new one — and reads 1980 → 1985.) After
it:

- the no-target Gamma (`95452c4`), +7: `tagpu_vk_world_record_direct`, `build_direct`,
  `direct_draw`, `direct_free`, `quad_buffer`, `quad_pipeline`, `target_down`;
- the bounded staging (`13b6b8a`), +17 −4: `tagpu_vk_stage.c` whole — the five non-statics
  `tagpu_vk_stage_begin`, `_drop`, `_expand`, `_copy`, `_down`, and `banded_one`,
  `banded_ready`, `barrier_in`, `barrier_out`, `copy_band`, `fill_copy`, `fill_expand`,
  `mem_type`, `mk_stage`, `resolve`, `slog`, `upload` — for `slot_drop_bstage` ×2 and the
  terrain's `shared_upload` and `slot_drop_bigstage`;
- the palette keys (`4143757`), none;
- the restorer's ring bound, +1: `vk_room` (§2.43);
- the dirty map and the repack's move (`9cde322`), +13 −1: `tagpu_gaf_dirty_since`,
  `tagpu_gaf_rects_due`, `tagpu_gaf_rects_area`, `mirror_move`, `serial_max`, `tiles_newest`,
  `tiles_raise`, `tagpu_vk_stage_expand_rects`, `tagpu_vk_stage_fits`,
  `tagpu_vk_stage_move_ready`, `tagpu_vk_stage_move`, `tagpu_vk_stage_move_drop` and
  `move_record`, for `tagpu_gaf_band_since`.

Every removed non-static had its callers ported; the build is clean under `-Wall`,
`thread-split-check.sh` is clean with the packet field gone, and `spirv-check.sh` agrees with the
regenerated headers.

**Device memory, per atlas.**

| | before G20c | after 2c | after 2d |
|---|---|---|---|
| feature, effect, unit atlas (2048² each) | 4 MiB R8 | 4 MiB R8 + 16 MiB base | 16 MiB base |
| terrain, Two Continents (2176 × 2720) | 5.6 MiB R8 | 5.6 MiB + 22.6 MiB | 22.6 MiB |
| all four, Two Continents | 17.6 MiB | 88.3 MiB | **70.6 MiB** |

That is 3 B/texel over the R8 alone, +53 MiB on Two Continents, and −17.6 MiB against 2c; the
restored twins are the same size in every column. The small per-slot images removed at 2d come
to about 10 KiB a slot. The unit pass's transient staging no longer reserves up to 4 MiB a frame
for an R8 upload. Its fragment-stage uniform block is 256 bytes, down from 272 now that `uLit`
is gone. At the reference device's 64-byte alignment that is 1 536 bytes a unit instead of
1 728, so the design point's blocks come to 23.0 MB a frame slot (§2.86).

**The staging: host-visible memory, bounded by a constant** (`tagpu_vk_stage.c`, the header
carries the argument). Every upload into a shared image — the base atlases of the feature, effect
and unit passes, the terrain's base atlas and height grid — goes through it.

- **Before, each slot staged its whole upload in one buffer.** A full page is a feature, effect or
  unit atlas's used rows × 2048 × 4, up to 16 MiB. The terrain's is its whole atlas, 2176 × 5338
  × 4 = 46 461 952 bytes on Town & Country, plus the height grid, all in one mapping. A refused
  allocation took the pass down for the session.
- **Now each slot keeps one `TAGPU_VKSTAGE` of at most `TAGPU_VK_STAGE_CAP`, 2 MiB**, made on a
  frame that uploads and given back on the slot's next frame with nothing to send. 2 MiB is the
  atlases' allowance, 1 024 tiles of RGBA8 (below), and `tagpu_vk_stage.c` asserts that the two
  agree. **The address-space bound** is a constant whatever the atlas: one buffer per slot per
  pass, and only on a frame that sends. The four world passes map at most **24 MiB** at the three
  frame slots the swapchain gives on the reference setup, and **64 MiB** at the `TAGPU_VK_SLOTS`
  ceiling of eight.
    - **In play an upload is in the frame's own command buffer, or it is not made.** The tiles an
      atlas painted since the device's copy go as one copy when they fit what the slot mapped
      (`tagpu_vk_stage_expand_rects`). The atlas's allowance is what makes them fit. When they do
      not — a slot that could map less than they need — nothing is recorded and the pass skips
      the frame. The seam's fence on the slot is what frees the buffer.
    - **A whole page goes out in bands**, and only on the events *What sends a whole page* lists:
      whole rows through the rest of the buffer, each band copied by a submit of the module's own
      command buffer to the seam's queue and waited on (its fence, one second) before the next
      band overwrites the same bytes. A wait that gives up leaves the buffer marked in flight, and
      it is not recorded again until the device reports that submit done.
    - **No frame samples a texel the device has not received.** An in-frame copy is ordered before
      the frame's draws by its barriers. A banded upload is complete — every band copied, the
      image back in `SHADER_READ_ONLY_OPTIMAL` — before `prepare` returns. Its submits reach the
      queue before the frame's own, so the frame that asked for the page draws all of it. The
      price is a wait on the CPU.
    - **A refusal never latches.** `tagpu_vk_stage_begin` halves its ask on a refused allocation
      or map, down to one row of the widest upload. A slot that cannot map what the frame needs
      gets 0, and the pass skips the frame without advancing its serials: the upload is still due
      and the next frame asks again. **A skipped frame draws nothing of that pass** — the
      terrain, the features, the effects or the units are missing from that one frame — rather
      than anything sampled from texels the device has not received. Only a device that fails a
      submit or a fence partway through a banded upload refuses the pass.
    - **A refused pass gives its restore job back in the same `prepare`** (`refuse_job`, in the
      terrain, feature, effect and unit passes, at every refusal). A band that failed partway
      leaves the base atlas half written and in `TRANSFER_DST_OPTIMAL`, and the job's FILL samples
      that image through a descriptor that says `SHADER_READ_ONLY_OPTIMAL`. The teardown that
      frees everything is paid at the top of the next frame, behind the seam's device drain
      (`tagpu_vk.c`), and this frame's restorer slice is still to be recorded, so the job cannot
      wait for it. The seam records the restorer's slice after every pass's `prepare`, so a job
      given back there is never recorded again. `tagpu_vk_restore_job_free` retires what a submitted
      buffer still names, which is what makes the free legal mid-frame.

  **MEASURED 2026-09-24**, `scenarios/big-battle.json` (Town & Country, 980 units converging),
  `--defaults`, Gamma 12, through the map load, a scroll burst (the camera jumped across the map
  nine times, 0.3 s apart), one forced swapchain rebuild (the path a window resize takes) and the
  fight. A diagnostic-only build summed each pass's mapped staging over its slots after every
  frame's prepares; the figure is the largest sum at one frame, in MiB. The cap was 1 MiB for
  this table; at 2 MiB the bound above is the statement:

  | | features | effects | units | terrain | all four |
  |---|---|---|---|---|---|
  | before, 3840 × 2160 | 14.75 | 5.11 | 2.21 | 44.59 | **66.63** |
  | before, 2560 × 1440 | 14.27 | 4.16 | 2.19 | 44.59 | **65.20** |
  | after, 3840 × 2160 | 1.78 | 2.09 | 1.01 | 1.00 | **5.09** |
  | after, 2560 × 1440 | 2.00 | 1.00 | 1.00 | 1.00 | **4.00** |

  **The banded path's cost**, timed from the first band's fill to the last band's fence, in the
  same runs (before the dirty map, so play still sent pages then):

  - **At 2560 × 1440** the terrain's whole atlas takes 63 bands in 6.6–6.7 ms. The worst upload
    took 9.3 ms, of which 6.9 ms was the first band waiting behind the frames still in flight.
  - **At 3840 × 2160** the first band's wait is up to 27.9 ms (28.7 ms in all). A fresh image's
    upload waits too, because its first band still queues behind the frames the queue is
    executing. The terrain's 63 bands take 6.0–8.2 ms. One of those runs went through 43
    render-thread restarts, each a full re-send. The restart loop predates this work: the G20c
    base build restarts the same way at 4K (below, *Not covered*).

  So a whole-page event costs the render thread a wait of up to 28.7 ms at 4K, in place of up
  to 66.6 MiB mapped at once — which is why no frame of play may take one.

**What sends a whole page — never a frame of play.** A whole page — a pass's whole used rows,
through the banded path, which waits — is sent on these events and no others:

- a pass's first upload after it is built, so the session's first level and every teardown (a
  swapchain rebuild, which is what a resize runs);
- the terrain's new atlas and height grid at a level's start;
- a palette move (the engine serial);
- an atlas made or lost, its mirror armed, or a new level begun on it
  (`tagpu_gaf_atlas_forget`, which writes the whole page, so a level opens with every visible
  frame at once, as it does with the terrain's new atlas).

Each pass logs the cause with the upload: `<pass>: the whole base atlas, N KB, through the banded
path - <cause>`, and the terrain `terr: the base atlas and the height grid, N KB, …`.
**The one route from play** is a feature repack on a device that refused the move buffer
(`tagpu_vk_stage_move_ready`): the pass then takes the repack as a whole page, and the line says
`a repack this copy cannot move - A WAIT IN PLAY`. A recycle — a full unit or effects atlas
dropped (`atlas_drop`) — writes nothing to the mirror; the live set is painted again as ordinary
paints, under the allowance.

- **The unit atlas recycles before the pose bake latches its generation**
  (`tagpu_r3d_atlas_recycle`, right after `tagpu_r3d_atlas_level` in `tagpu_native_frame`).
  A material the bake cached is reused while its stamp equals the generation
  `tagpu_posebake_frame` latched. A recycle after the latch left every cached material matching
  for the rest of that frame, drawn through rects the frame's new paints were overwriting: the
  units in the wrong texels for one frame per recycle. Before it, every stale material drops on
  the latch's line. `tagpu_r3d_atlas_frame` keeps only the allowance's frame.
- **An effects recycle draws the base atlas on its frame.** A recycle is a new list generation,
  and the effects pass replaces its restore job on it; the frame's `uRestored` is decided after
  that (below), so the new sprites at recycled rects are never drawn through the old sprites'
  restored texels.

- **In play a pass sends the tiles its atlas's dirty map names** (`tagpu_gaf.h`): for each 32 × 16
  tile, the mirror serial of the last write to it. The map has a slot per tile, not per write, so
  it cannot overflow: however many paints land between two uploads, every tile they touched is
  named, and a burst of paints is never a whole page. The tiles go as one copy of up to 1 024
  regions (`tagpu_vk_stage_expand_rects`, `TAGPU_VK_STAGE_MAXRECT`), in the frame's own command
  buffer, **never banded**. A mirror wider than the map's 2048 is refused at its arm.
- **The allowance** keeps them inside one slot's staging. Each of the unit, feature and effects
  atlases carries `budget` = `TAGPU_GAF_BUDGET`, **1 024 tiles = 2 MiB** of RGBA8, sized from the
  largest single-frame paint measured in play before it, 1.20 MB after a camera jump at 4K, with
  headroom (the re-measure below saw 1.79 MB in one frame right after a level's load).
    - **The consumer says what it holds.** After every upload, and on every frame whose copy is
      current, the pass acks the serial it holds (`tagpu_gaf_atlas_ack`); a new image or a
      teardown acks nothing. `due` is the count of tiles written after that serial: every mirror
      write (`mirror_wrote`, `tiles_raise`) adds the tiles it makes due, and every ack recounts.
    - **The gate** (`gate_paint`, in `atlas_insert`): a paint that would take `due`, plus the
      tiles already waiting ahead of it, plus the tiles it would make due, past the allowance is
      **deferred**. The entry keeps its rect, reserved; `tagpu_gaf_atlas_get` returns NULL and
      `deferN` moves. So `due` never exceeds the allowance, and the upload never exceeds the
      staging, by construction. The gate is off while the consumer holds no copy or is behind a
      whole-page write, when its next upload is the page anyway.
    - **What is not drawn that frame**, so that nothing is drawn from texels the device lacks: a
      feature's whole anchor (body and shadow roll back, `tagpu_feat.c`); an effect's whole
      record — a projectile with its ground shadow, an explosion with its flash and its body — when
      any sprite of it is deferred, or any other part of it is lost for want of room (below)
      (`effect_begin`/`effect_end`, `tagpu_fx.c`: the buckets, the
      quad count and the model list go back to where the record started, so a flash never draws
      without its explosion or a shadow without its shell; the explosions are one walk, flash and
      body per record, and B_FLASH still draws before B_SPRITES); a unit whose bake needs a deferred face
      (`tagpu_r3d_atlas_uv` answers −1, `mat_bake` refuses, and the unit is skipped for the frame).
      Nothing is drawn from content the device already holds in its place: a deferred entry has
      no content anywhere yet.
    - **An effect loses its whole record for any part it loses, not only a deferred one.** Every
      emitter return in `tagpu_fx.c` that leaves an asked-for part unwritten counts one
      `s_partsLost`: `tagpu_gaf_atlas_get` answering NULL (a deferral, the atlas full, a decode
      that failed) and a full bucket in `put_quad` or `emit_line`. The compound sprite
      (`emit_sprite`) and the record bracket roll back when that count moved. Keyed on the
      deferral count alone, a sprite the full atlas refused was dropped by itself — an atlas that
      fills partway through a gather still answers its cached frames — and the rest of its record
      drew. A frame the GAF reader refuses, or one nested past four levels, is not counted: that
      is the frame's own shape, the same on every frame.
    - **Why no frame samples an unreceived texel.** Every entry a frame draws was painted before
      that frame's hand-over, so its tiles are at or below the serial the hand-over carries. The
      pass brings its copy to that serial in the frame's own command buffer, ordered before the
      draws, or skips the frame. A deferred entry is not drawn at all.
    - **First refused, first painted, and it drains.** `tagpu_gaf_atlas_frame`, after the reset
      and before the first lookup of every frame, puts each entry still waiting — and asked for
      on the frame before — into a bucket by the frame it was first refused on (at most eight,
      oldest first; past eight a cell joins the next younger bucket). A new entry counts every waiting tile
      against the allowance, a waiting one only the older buckets, so nothing waiting is
      overtaken by anything refused after it. With nothing due at a frame's start — the
      previous frame's upload acked — the oldest asked entry always paints: the largest cell
      touches at most 22 × 42 = 924 tiles, which `tagpu_gaf.h` asserts fits 1 024. An entry
      nothing asked for on the frame before stops waiting and is plainly reserved again. When the
      last waiting entry lands, the atlas logs `every deferred paint has landed` with its tallies.
    - **The consumer never waits on a draw.** The gate waits on the consumer, so a consumer that
      uploaded only when it drew could wait on the gate for ever — a frame whose every feature is
      deferred draws nothing. Each pass therefore uploads after its prepare on every frame its
      atlas says it is owed one (`tagpu_gaf_atlas_owed`, the `atlas_owed` step of
      `tagpu_vk_feat_prepare`, `tagpu_vk_fx_prepare` and `tagpu_vk_unit_upload`), drawn or not.
    - **A repack waits for a current consumer.** `tagpu_gaf_atlas_reset` holds a full atlas
      while the consumer is behind the mirror, so a moved cell never carries a due serial to more
      tiles than it had, and the move adds nothing due.
- **A feature repack moves its cells** rather than repainting them:
    - `mirror_move` (`tagpu_gaf.c`) carries each painted cell to its new rect in the mirror.
    - The feature pass moves its own device copy the same way (`tagpu_vk_stage_move`): the
      rows the old cells occupy go into a 16 MiB device-local buffer in one copy, and each cell
      comes back at its new rect. Nothing of the move comes from the host. It is recorded in the
      frame's own command buffer, before that frame's paints.
    - The dirty map gives each moved cell's new tiles the newest serial its old tiles held, so a
      paint the copy lacked before the move is still sent after it, at the new place.
    - A pass that cannot get the buffer takes the whole page instead: the one wait in play above.
    - An entry that was never painted is reserved and painted on its next use, as before
      ([atlas-packing](../atlas-packing.html) §7).
- **...and the Classic++ restored twin moves with them.** The twin's texels exist only on the
  device, so they cannot be sent again; before this they were blanked at every repack and
  restored anew, and the features drew unrestored for the frames that took.
    - **The published restore list keeps its generation** (`tagpu_gaf.h` `rlistGen`). An entry
      knows its frame's index (`rli`), and `rlist_moved` rewrites each moved entry's frame in
      place at its new rect and blanks the frame of an entry the repack dropped (w and h 0, which
      a job skips). The consumer's cursor into the list stays good.
    - **The feature pass carries the twin** (`twin_move`, `tagpu_vk_feat.c`), straight after
      the base atlas's move in the same command buffer:
        - the restore job's queued frames move by the same list, and a frame whose entry was
          dropped is dropped (`tagpu_rcore_job_remap`);
        - the batch in flight, whose FILL read the old rects, goes back to the head of the queue,
          so nothing in flight spans the move;
        - the twin's cells move on the device through the same buffer
          (`tagpu_vk_stage_move_twin`), with everything no cell lands on cleared to 0 in between.
          Alpha 0 is what the feature shader reads as "not restored, take the base", and a rect
          the repack freed must not show another entry what the old one left there.
    - **Why the twin cannot disagree with the base.** Both move by one list in one command
      buffer. The job's frames are rewritten before the restorer records its next slice, which
      it does after every prepare. And nothing in flight spans the move. So after it no FILL
      reads the base, and no OUT writes the twin, at a rect of the old layout. A twin the job has
      not drawn into yet is not moved: the job's first draw clears it.
    - **Only what was never restored is restored after a repack.** Anything that stops the twin
      following — a base atlas that took the repack whole, a job that cannot take its batch back,
      a twin the device cannot move — drops the job, logged. The frame it happens on already draws
      the base atlas (the flag below), and the next job blanks the twin and restores from the
      list's start, the rule for any discontinuity.
- **`uRestored` is decided after the upload and `restore_want`, from what the frame samples.**
  `restore_want` makes and feeds the job; the job itself paints in `tagpu_vk_restore_step`, after
  every pass's prepare. In the feature and effects passes both `base_upload` and `restore_want`
  can take the twin away in the frame itself: `twin_drop` on a repack the base took whole or the
  twin could not follow, and a replaced job on a new generation (an effects recycle) or a move
  of the engine's table. So the flag is read after both, from `s_arHave`, which every path that
  disowns the twin clears first — `restore_want` as soon as the old job is gone, before any of
  its returns, so a new generation whose job cannot be made does not keep the old picture's flag
  either. The unit pass decides its flag after its `restore_want` too, and writes the per-unit
  blocks that carry it after that; the terrain decides its flag after its own.
- **A twin is bound only while it is a picture.** Every twin is created `UNDEFINED`, and only
  its job's OUT leaves it `SHADER_READ_ONLY_OPTIMAL`; `s_arHave` comes from `painted > 0`, so it
  says exactly that. The feature and effects passes bind their twin (40, 42) for the slot in
  hand on every prepare that draws, after the flag, only when `s_arView && s_arHave`, and the
  base atlas's view otherwise (`twin_bind`); `atlas_build` starts the binding on the base view.
  The units' `bind_main` (41) and the terrain's `shared_bind` (41) keep the same rule; the
  terrain's runs after its `restore_want` in `prepare`, because the frame on which the first cell
  is painted is the one where an earlier bind and the flag would disagree — the shader would
  read the tile atlas at the neighbourhood atlas's key cells, one frame of the wrong tiles
  (measured on the reference setup: one frame, ~7 after the first world frame of every load).
  So no descriptor names an image in a layout it is not in — all of Classic, where no job runs,
  included — and in every pass the flag and the binding come from the same painted-flag (`s_arHave`; the terrain's is `s_rgbAtlas.have`).

**MEASURED 2026-09-24 — what play sends.** The setup:

- `scenarios/big-battle.json`, `--defaults`, 3840 × 2160, Gamma 12, on a private virtual display
  (Xvfb) with the reference setup's discrete GPU.
- A scripted minute after the load:
    - 20 s of the fight's start with the camera pinned;
    - the pointer on each screen edge for 3 s (edge scroll);
    - nine camera jumps across the map, 0.5 s apart;
    - eight wheel notches out, an edge scroll while zoomed out, and eight back in;
    - 20 s on the fight.
- A diagnostic-only build logged every base-atlas upload with its pass, cause and bytes, and
  every banded upload's wait. Two runs of each build.

| in play, each run | before (`6511def`) | the dirty map and the move (`9cde322`) | the allowance and the twin (`c2929cb`) |
|---|---|---|---|
| full page, band-ring overflow | **1** and **1**: the features, 140 and 120 paints in one frame at the repack, 10.1 and 9.2 MB, 4.3 and 3.9 ms | **0**: the cause is gone | 0 |
| full page, any other cause | 0 | 0 | 0 |
| a partial upload over the cap (banded) | **12** and **11**: 1.1–13 MB sent, for 30 KB–2.2 MB painted in the run that logged the paints | **1** and **1**: 1.41 and 1.36 MB sent for 1.20 and 1.18 MB painted, waits 0.34 and 0.20 ms | **0** and **0**: the path no longer exists |
| **waits in play, in all** | 13 and 12 | 1 and 1 | **0** and **0** |
| the largest upload in play | — | 1.41 and 1.36 MB, banded | 1.79 and 1.79 MB, in-frame: the features' first paints after the load |
| frames deferred by the allowance | — | — | 0 and 0 (0 bytes) |
| frames skipped for want of staging | — | — | 0 and 0 |
| a feature repack | 1 and 1, sent as the overflow above | 1 and 1: 234 and 270 cells moved on the device | 1 and 1: 256 and 269 cells moved, **the twin with them** |

The only waits of the last two runs are the level's first uploads, before play: the terrain's
46.5 MB in 6.1 and 5.9 ms, the features' 6.1 MB in 1.1 ms, each logged with its cause.

- **Before, the partials were the ring's bounding box**: 64 rects and a union, so scattered
  paints sent the rectangle around them. Several came in the seconds after the repack, as the
  reserved entries were repainted one by one.
- **The dirty map alone** (a run without the move) took the overflow out and left three banded
  partials, all within two seconds of the repack: 0.88–2.9 MB painted, the reserved entries
  repainting.
- **Moved only in the mirror**, the repack is a whole-page write, 10.8 MB, banded. That is why
  the device moves its own copy.
- **The terrain sent nothing in play.** The units and the effects stayed in-frame throughout.
- **The move in the mirror costs the render thread 3.9 ms** at 270 cells, on the reference setup
  with other game instances running beside it.
- **The device copy is exact.** A diagnostic-only build of `9cde322` read the feature base back
  off the device on the three frames after each repack and on every 600th frame. It compared it
  texel for texel with the mirror expanded through the same palette, over every painted cell no
  paint had touched since: 236 and 281 cells after the two repacks and 157 on a control frame,
  with **0 texels different**.
- **The waits are the virtual display's.** On the live 4K display the first band also waits
  behind the frames in flight, up to 27.9 ms (above).

**A refusal degrades and recovers.** Big-battle at 2560 × 1440; mid-fight, a forced swapchain
rebuild, so every pass re-sends its atlases, together with 120 forced refusals of
`tagpu_vk_stage_begin` (diagnostic-only). The passes skipped frames while refused, no pass
refused or tore down, and the census before and after read the same passes drawing
(`terr=1 feat=1 unit=1 fx=1`).

**MEASURED 2026-09-24 — the deferral, forced.** In the minute above the 2 MiB allowance was never
reached, so a diagnostic-only build of the same code cut it to 384 tiles (768 KiB) for all three
atlases and ran the same minute:

- **The features deferred on 3 frames, 1 286 KB first deferred in all** — two events. At the
  level's start the first paints after the load asked for 58 cells: 38 refusals in one frame, 33
  entries and 862 KB waiting; the next frame painted 22 of them and the one after the rest.
  Later a camera jump deferred two cells, 424 KB, for one frame.
- **Both drained**, each logged `every deferred paint has landed`: the longest wait 2 frames,
  35 entries painted after waiting, none gone unasked.
- **Waits in play 0, frames skipped 0.** The units and the effects never reached even 384 tiles.
- The twin followed that run's repack exactly as in the runs below (252 cells, 20 restored,
  identical).

**MEASURED 2026-09-24 — the twin follows the repack.** A diagnostic-only build read the
features' restored twin back off the device just before each repack's move and again on the next
frame, and compared every moved cell that held restored texels before with the same cell at its
new rect, texel for texel. It also counted, when the restore job next went idle, the frames
painted since the move against the frames queued at it plus the frames added since, the
difference being any frame restored twice.

| | repacks | cells moved | restored cells carried | identical | queued frames moved | in flight, re-queued | dropped with their entry | restored twice |
|---|---|---|---|---|---|---|---|---|
| the scripted 4K minute, two runs | 1 and 1 | 256 and 269 | 20 and 20 | **all** | 236 and 249 | 0 | 0 | — |
| big-battle 1920 × 1080, a repack forced every 30 frames | 47 | 3 384 | 1 781 | **all** | 1 570 | 33 | 359 | **0** in 24 checks |
| `feat-forest` 1024 × 768, a repack forced every 400 frames | 16 | 178 | 162 | **all** | 16 | 0 | 0 | **0** in 14 checks |

- **No texel outside the moved cells was left non-zero** after any of those moves, so the clear
  holds: a rect a repack freed shows the base atlas until its new entry is restored.
- **At 4K the twin is mostly unrestored when the repack comes** (20 of 256 cells): the terrain's
  restore job has the first claim on the restorer and 10 036 frames to paint at about six frames
  a second on the virtual display. The moved queue is what carries the rest.
- **No job was dropped** in any run: every repack's move was followed.

**MEASURED 2026-09-24 — the review's residuals.** Diagnostic-only builds of the fixed code, on the
private virtual display, `--defaults` (Classic++, so every restore job runs), Gamma 12:

- **The twin the frame samples** (the feature and effects passes). The diagnostic counted every
  frame drawn with `uRestored` 1 while the pass's job was gone, had painted nothing, or belonged to
  another generation or palette. It also counted the same for the flag as the old order decided
  it, before `base_upload`. Big-battle at 1920 × 1080, upplay's scripted minute. The features
  repacked every 30 gathers, every other repack refusing the move buffer: 27 refused — 26 of
  them whole pages that dropped the job, the last the band failure below — and 26 carried. The
  effects atlas recycled every 45 gathers (50 recycles), the unit atlas every 60 frames (29). The
  build is `7c0f25c`'s; the flag it counts is decided the same way at the tip:

  | pass | frames drawn | `uRestored` 1 | against a twin it does not own | the old order |
  |---|---|---|---|---|
  | features | 1 503 | 832 | **0** | 15 |
  | effects | 1 984 | 1 096 | **0** | 30 |

- **The unit atlas's recycle.** In the same run, 657 895 cached-material hits over the 29
  recycles, **0** with a latched generation other than the atlas's. The control, the same
  build with the recycle moved back after the latch: 8 150 stale hits over 21 recycles.
- **A band that fails.** With a file probe, the next banded upload failed on its second band: the
  features' whole page after a refused move, 279 rows. The shipped log shows the refusal on that
  frame — `stage: a banded upload failed on the device at row 256 of 279`, then `feat: slot 1
  would not take this frame's resources - the pass stops drawing and the seam tears it down` —
  and `a pass asked to come down: the device is drained, tearing it down` at the top of the next
  frame. `refuse_job` logs nothing; the diagnostic build's own line showed the pass's job given
  back in the refusing `prepare`. The terrain, units and effects drew on (the census), the
  effects' restore kept painting, and nothing faulted.
- **Half-drawn effects.** The diagnostic tagged every vertex with its record, and after the
  gather counted the records that had a sprite deferred and still had a vertex in the
  hand-over:
    - **At a 384-tile allowance** (all three atlases, the 4K minute) no effect deferred: the
      effects never reach 384 tiles. The features deferred on 4 frames and drained.
    - **With the effects' allowance cut to 16 tiles** (the units' to 32, the features' 384),
      big-battle at 1920 × 1080: 160 572 records, 9 191 with a sprite deferred. 8 428 of those
      had another part that the per-sprite rollback would have drawn alone. **0** records with a
      deferred sprite had any vertex in a hand-over.
- **An effect that loses a part for want of room** — the atlas full, a bucket full — and not by
  a deferral. A diagnostic build of `8007b4c` held the effects atlas to 48 entries, so that it
  fills partway through a gather — its cached frames still answer, new ones fail — and the next
  gather's recycle empties it. Big-battle at 1920 × 1080, the scripted minute. The diagnostic
  counted, per record, the parts asked for (a leaf sprite or a line past the mute) against the
  parts written, and after each gather the records that had lost one and still had a vertex in
  the hand-over. The control rolls back on the deferral count alone, as the code before did:

  | build | frames | records | atlas full | records that lost a part | half-drawn in a hand-over |
  |---|---|---|---|---|---|
  | the tip | 1 680 | 147 542 | 434 | 26 895 | **0** |
  | the control | 1 705 | 151 835 | 425 | 22 279 | 1 538 (9 228 vertices) |
  | the tip, and every 400th quad and 150th line refused as a full bucket | 1 620 | 136 469 | 453 | 18 908 | **0** |
  | the control, the same | 1 692 | 154 798 | 530 | 21 111 | 1 882 (11 292 vertices) |

  The allowance deferred nothing in these runs, so every lost part was one the deferral count
  could not see. Most records that lost a part had no other part to draw; the rest are the
  control's half-drawn ones.

**MEASURED 2026-09-23** — Two Continents, `scenarios/tascene-parity.json` (terrain, units) and
`feat-forest` (features), 1024 × 768, `ss=2`, the world A/B read off the target
(`tagpu_<pass>.ab`), per preset; `los 0` is no fog, `los 1` the fog grid:

- **2a, the base beside the R8:** 0 px on every pass in both presets at Gamma 12 and 16; the
  restore dump (`tagpu_restoredump.on`) is identical to the build before on the terrain, 923 of
  923 unit frames and 3323 of 3323 feature frames (the effects 137 of 140, whose frames arrive by
  the fight's timing).
- **2b, Classic onto the full-colour path, old Classic against new at Gamma 12:** terrain 0 px
  without fog; with fog 1 710 518 px differ (max 10), **1 710 306 of them in the grey band** and
  212 outside it (max 4, the band's edge); features 0 px without fog, 322 724 with it (311 556 in
  the band, max 10); units 1 022 of 1 540 drawn pixels (max 46) — the multiplier against the
  table's nearest-colour remap. Classic++ moved on the units only (1 540 of 1 540, max 87: they
  take the face shade now); terrain and features 0 px. The lab's parity pair moved on the units
  only, 330 px at the default and 256 at `ss=1` ([renderers](../renderers.html) §1, the new md5s).
  The owner's side-by-side PNGs were shot at 1× and zoomed, in and out of fog.
- **2c, the Gamma once:** at Gamma 12 every capture is **byte-identical** to 2b's — terrain and
  units with and without fog, features with and without fog, both presets. The sweep at Gamma 0,
  6, 16 and 20 and the live change are in §2.3f.
- **2d, the 8bpp path deleted, against 2c at Gamma 12 and 16:** terrain, units and features
  **byte-identical** in both presets, with and without fog at 12 and with fog at 16 — 18 of 18
  captures.
    - **The effects** cannot be paused on the same tick in two launches: the fight's timing moves
      by a tick or more between runs. So every 2c capture was paired against every 2d capture, and
      two pairs show the same scene — 2c's tick 1023 against 2d's 1022, and 1485 against 1484.
      In Classic both are 0 px, over 5 152 and 4 156 drawn pixels; the second pair has a flash
      queued. In Classic++ the first pair is 0 px. The second has 16 px at max 1: eight near-black
      `[1,0,1]` texels drawn at eight other positions, with the same colour multiset. Classic draws
      the same texels as `[0,0,0]`, the clear, so the move cannot show there. It is the scene a
      fraction of a tick apart, not a colour. The colour cannot move by construction: the base
      holds the engine's table entry for each index, and the flash level comes back out of the alpha
      exactly.
    - **The markers** cannot be captured alone: their selection rects need the unit pass up, and
      the A/B refuses a frame that two passes drew. So they were read off the presented window
      (`import -window`, 1024 × 768, `ss=2`, Gamma 12), in both presets.

        - **First fixture:** `selbox-facings`, three units selected, fog on, paused. The whole
          window is byte-identical between the two builds.
        - **Second fixture:** `scenarios/marker-mix.json` (MEASURED 2026-09-24). Tanks at 30, 65
          and 100 % health and a Commander at 80, on Two Continents, with water and the fog band
          in view, paused. Five states, each read twice with both reads agreeing:

            - every unit selected, which shows the selection rects, the four health bars and
              group digit 1;
            - SHIFT held with a patrol queued, which shows the order lines and dots, with a tank
              hovered for its route dots;
            - a held drag, which shows the band box;
            - the Commander's solar collector, showing the placement ghost, its footprint and a
              SHIFT-queued site with its ghost;
            - all of that at zoom 1.5.

            Old was 2c with main merged in, new is the branch tip, and all ten pairs are
            **byte-identical**. The effects passes were off (`fx.off sfx.off`) and the UI layer
            was off (`gui.off`). Health bars need the `damagebars` registry value, which every
            instance shares and which nothing may write, so both DLLs carried a diagnostic
            `mark.on` token that opened that gate alone. It was compiled for this test and is
            not in the tree.

        - **With the effects on**, every state differed in one 19 × 19 box over the 65 % tank: a
          smoke puff that one launch drew and the other did not (162 px at zoom 1, 462 at 1.5).
          It is the damaged unit's smoke at another point of its life, which is effects and not
          markers.

    - **The lab:** its Classic lane, now on the base atlas and `k`, draws byte-identical to 2b's.
      Its Classic++ and relief-sun pictures are unchanged.

- **A frame with no world target, after 2d (MEASURED 2026-09-24).** The target path and the
  no-target path were compared at `ss=1`, so both draw at the same resolution, on the reference
  setup's `B8G8R8A8_UNORM` surface. The fixture was `selbox-facings`, three tanks selected, fog
  on, paused, read off the presented window. The no-target path was forced by a
  diagnostic-only file probe in `prepare`, which is not in the tree.

    - **Gamma 12:** the target, the no-target path, and the no-target path without the quad are
      all byte-identical over the whole window.
    - **Gamma 16 (factor 1.1667):** the target is 100 % the engine's curve
      `min(255, trunc(e × f))` of the unscaled world, using the factor as the engine stores it.
      Against it, the no-target path is exact on 69.6 % of the world's channels and one level
      off on the rest. Without the quad it was 1.4 % exact and up to 36 levels off.
    - **Gamma 6 (factor 0.75):** 75.2 % exact, the rest one level off. Without the quad it was
      up to 64 off.
    - Outside the viewport: 0 px in every case.

- **Every palette-keyed source rebuilds on a move of the engine serial (MEASURED 2026-09-24).**
  Big-battle at 2560 × 1440, `--defaults` (Classic++, so all four restore jobs run). A
  diagnostic-only file probe bumped `tagpu_pal_engine_serial` once mid-fight without changing
  the table's bytes, as a writer the survey missed would. Every source it keys rebuilt on that
  frame:
    - `k[]` from the engine SHD (`r3d shade:`);
    - the LHT flash colours;
    - the four base atlases, re-sent whole: units 2048 × 280, the terrain, features 2048 ×
      1374, effects 2048 × 532, each "palette serial 1 → 2";
    - new restore jobs for the units, the terrain, the features and the effects.

  Every pass kept drawing (the census), and the terrain's twin was repainted.

- **The bounded upload and the palette keys draw what the build before them drew (MEASURED
  2026-09-24).** The build before is `b14e9b4`; the fixtures, the capture and the settings are
  2d's above (Gamma 12, `los 1`, both presets, read off the target):
    - **Terrain, units and features:** 6 of 6 captures **byte-identical** — the terrain over
      2 523 124 drawn pixels, the units over 1 550 and the features over 529 468. The terrain's
      atlas is 23.7 MB, far over the cap, so it reaches the device through the banded path.
      With the restorer's ring bound (§2.43) added, the same 6 of 6 are byte-identical again.
    - **With the dirty map and the repack's move:** the same 6 of 6 byte-identical to
      `b14e9b4` again, both builds captured on a private virtual display at 1024 × 768 — the
      terrain over 2 523 124 drawn pixels, the units over 1 550, the features over 529 468. No
      fixture repacks, so the move rests on the read-back above (*what play sends*).
    - **With the allowance and the twin move** (`d55b18d`): the same 6 of 6 byte-identical to
      `b14e9b4` once more, on the same private virtual display. No fixture defers or repacks, so
      those rest on the runs above (*the deferral, forced* and *the twin follows the repack*).
    - **With the review's residuals fixed** (`e928ce3`): the same 6 of 6 byte-identical to
      `b14e9b4`, on the same private virtual display. The fixes act on forced events, so they
      rest on *the review's residuals* above.
    - **With the twins bound only while they are pictures, the units' flag decided after
      `restore_want`, and an effect rolled back for any lost part** (`8007b4c`): the same 6 of 6
      byte-identical to `b14e9b4`, on the same private virtual display. The effect rollback acts
      on lost parts, which no fixture has, so it rests on the atlas-full runs in *the review's
      residuals* above.
    - **The restorer's input and output:** `fx-rockets` under `--defaults` with
      `tagpu_restoredump.on`, read back off the device. The terrain's base and restored twin
      (23 674 880 bytes each) are byte-identical between the two builds. So are the units' base
      and their twin with its mip chain (22 020 096 bytes), in two launches of the build with the
      ring bound (§2.43). The feature, effect and UI dumps differ in which frames the fight had
      painted by then: 849–860 feature frames in one launch and 917–931 in another. Those dumps
      compare nothing.
    - **The effects have no exact A/B.** Three ways were tried: every capture of one launch
      paired against every capture of the other, the fight stepped five ticks at a time through a
      window in both launches, and the device dumps above. No pair showed the same scene, because
      the fight is not the same fight twice. The effects' change is the features' change,
      line for line: the same `tagpu_vk_stage` calls and the same restore key, with its own
      alpha table passed through unchanged. The flash colours are built by the loop they always
      were, and only what triggers the rebuild moved, so on a table that does not move they are
      the same bytes.
- **ShowRanges labels and the post-fog draws (MEASURED 2026-09-24).** This uses the
  `marker-mix` fixture and the old/new DLL pair of the second fixture above, on the presented
  window. Every unit is selected with a patrol queued, and `+showranges` is typed in, which
  `*0x511DE8+0x391BF` then reads as 1:
    - **The band box and the build square over fogged ground**, both drawn after the fog with
      the fog off: **byte-identical** in both presets.
    - **The labels state:** `mark:` counts 16 labels in both launches. Everything is
      byte-identical except 8 px in each preset, all inside one 14 × 14 box at the patrol
      waypoint's sprite. Its arms are one pixel longer in one launch.
    - That sprite is the order's target sprite (`0x439740`), and it is animated. Its frame is
      `(gameTime / (2 × (u16)seq[0x2C])) % (u16)seq[0]` (`tagpu_order.c`), and the two launches
      were paused at ticks 1887 and 1871. Stepping both
      onto one tick did not work: a step is five ticks, and the old build overshot its target.

**Not covered.**

- **The UI stays indexed**, through the presented palette, with its own re-restore on a palette
  change; nothing here touched it.
- **Past factor 1.0 a near-white shaded face and a clipped grey move by more than a level**
  against 2b; that is 2b's order being wrong (§2.3f), not a residual, but no engine A/B at a
  factor other than 1.0 has been run on the units.
- **The lab's Classic++ lane keeps its own R8 inputs** beside the restored RGB (its `restore=`
  switch and the edge colour read them); only its Classic lane lost the index path.
- **The effects' 2d exit rests on two matched scenes** (above), not on an A/B paused on one
  tick, which two launches of the fight cannot reach. **The bounded upload, the palette keys
  and the dirty map have no effects A/B at all**; that rests on the features' byte-identical
  pair and on the code being the same.
- **The markers' 2d exit is a window read at `ss=2` and Gamma 12.** The health bars were opened
  by a diagnostic token rather than by `damagebars` itself. The ShowRanges state was read at two
  different ticks, so its 8 px at the waypoint sprite are put down to the animation by where
  they are, not by a pair taken on one tick.
- **The deferral has not been needed in play yet.** In the scripted 4K minute no frame's paints
  reached the 2 MiB allowance (1.79 MB at most, right after the load), so its path is measured
  only under a diagnostic build with the allowance cut to 384 tiles (above). A frame's paints are
  bounded by the atlas, 16 MiB, so a scene denser than big-battle's can defer in play. What that
  costs is a feature, an effect or a unit drawn one or more frames late, never a wait.
- **No lane drew the effects' models** — a rocket's body and its flame, a shell, a debris piece.
  Reads of the presented window on `fx-rockets`, 0.05 s apart (MEASURED 2026-09-24), showed the
  smoke trails and the impact flashes and no rocket at a trail's head. §2.89 draws them.
- **A frame that fails to decode was not forced.** It answers NULL at the same line as the full
  atlas (`tagpu_gaf_atlas_get`), so it feeds the same `s_partsLost`, but no run made one fail.
- **No validation layer has run on the twin bindings.** That no descriptor names a twin in a
  layout it is not in rests on the code — `s_arHave` is set only from `painted > 0`, which
  counts OUT draws already recorded on an earlier frame, each of whose render passes leaves the
  twin `SHADER_READ_ONLY_OPTIMAL` — and on the A/Bs.
- **The twin move covers the feature atlas alone**, the one atlas that repacks. The unit and
  effects atlases recycle when full, which is a new list generation and blanks their twins as
  before.
- **The batch in flight at a repack is restored again from its FILL**: at most one batch of
  frames that had not been painted yet — 33 frames over the 47 forced repacks above.
- **A level now opens with the feature, effect and unit atlases' whole pages**, a wait at the
  level's start as the terrain's already was: 5.9 MB for the features on big-battle, 1.1 ms on
  the virtual display.

- **The restorer's ring bound (`vk_room`) was measured on one fixture and one device**:
  `fx-rockets` on the reference setup, in the launches listed in §2.43.
- **The UI layer stood itself down on some launches of a fresh instance** ("`gui: 8 fresh starts
  have not made the twin store able to follow the producer`", census `gui=0`). The pre-G20 base
  build did the same on the same instance, so it predates this work. It was not investigated
  here. The marker A/B turns the layer off in both runs so that it cannot decide the diff.
- **The restorer's wrap-pad decision is not re-keyed on a palette move.** Whether a frame is
  tileable (`tagpu_rglsl_tileable`, a colour distance over the engine's table) is decided when
  the frame is painted (`tagpu_gaf.c`) or, for the terrain, when the restore list is published
  (`tagpu_terr.c` `restore_publish`), with the table of that moment. A move of the serial starts
  a new restore job, and the job repaints the new colours with the old classification. The
  table has one writer and does not move in play, so this is the same hypothetical as the keys
  above, not an observed fault.
- **The render thread restarts in a loop at 3840 × 2160 on some launches**: 43 restarts in one
  staging run, each a `vk_down` and a full re-upload. The G20c base build (`78c22b2`) does the
  same at 4K, so it predates this work. It was not investigated here.
- **The no-target Gamma on an sRGB surface** misses the curve by far more than a level (§2.3f).
  Only the fallback surface is sRGB, and no run has used one.

**Risky spots for a review.** The packet layout (a field removed from the game-to-render
hand-over; every offset after it moved); each world pass's binding list and uniform offsets
against its regenerated header; the base images shared across slots and written behind a barrier
from per-slot staging; `base_upload` as the one atlas upload and its dimension guards (feature,
effect and unit), which with the terrain's retire are what keep a shared image alive while frames
in flight sample it; `tagpu_vk_stage.c` — the cap, the halving, the banded path's own command
buffer on the seam's queue, its one-second fence and the in-flight mark a wait that gives up
leaves, and the 1/0/−1 answers each pass maps to draw, skip and refuse; the dirty map —
`mirror_wrote` as its one writer, the 2048 guard at the mirror's arm, `tagpu_gaf_dirty_since`'s
merge window and its last rect grown past `maxr`; the repack's move — `mirror_move`'s bounds and
its serials read (`tiles_newest`) before any is raised, the consumer's test in
`tagpu_vk_feat.c` `base_upload` (`atlasMovePrev`, and `s_bMove`, which keeps a move from being
applied twice), and the move buffer made once per pass life and dropped in `_down`; the
allowance — `due` kept by `mirror_wrote` and `tiles_raise` and recounted on every ack, `gate_on`'s
conditions, the backlog's buckets and `bk_ahead`'s "older than" test, the frame hook's place
(after the reset, before the first lookup), the repack held while the consumer is behind, the
owed upload after each prepare (`s_upFrame`, and the ack of 0 at every `atlas_build` and `_down`),
and each caller's rollback of a deferred lookup (the features' anchor, the effects' record
bracket — keyed on `s_partsLost`, which every emitter drop feeds — and its one-walk explosions,
the units' bake); every world pass's `uRestored` read after its `restore_want` (the units'
per-unit blocks written after it), `restore_want` clearing `s_arHave` before its returns, and the
feature and effects twins bound per slot from the same `s_arHave` (`twin_bind`); `refuse_job` at each world pass's refusals; `tagpu_r3d_atlas_recycle` before
the pose bake's latch; the twin move — `rlist_moved`'s in-place rewrite of frames a consumer already
took, the job's remap and the in-flight batch put back at the head of its queue,
`tagpu_vk_stage_move_twin`'s barriers (the colour-attachment source scope, the clear before the
cells), the job dropped when the base atlas took a repack whole, and `restore_take`'s cursor over
blank frames; the palette keys
(`s_rjPal`, `s_shadePal`, `s_lhtPal`) and `tagpu_r3d_shade_want`'s kept copy of the shade table;
the restorer's `room` — asked at the head of every iteration of the core's slice loop, before a
batch is formed, with `vk_room`'s mip reserve and the static bound that keeps a fresh slice able
to draw; the flash level in the effects' alpha; the
restorer's input switched to the base; the Gamma curve's per-slot images and the premultiplied
composite; `k[]` and its fallback; the `shadows=0` change. The no-target Gamma: the per-frame
choice between the two arms (`prepare`'s 0 and `record_direct`'s `s_drawThis` guard), the blend
constants and the doubling above 2, and the teardown split — `_down_paid` now frees the target
alone and `_down` both.
