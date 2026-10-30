# The build ghost

The translucent unit at the placement cursor.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.23 The build ghost (`tagpu_native.c`, a play default since 2026-09-14, `tagpu_ghost.on`) — 2026-09-12

A translucent copy of the building under the placement cursor and of every queued build the order
pass is showing a site rect for, drawn through the posed program in the **model's own colours**
at the ghost's alpha, over the footprint squares exactly as the mark and order passes draw them.
The squares alone carry the green/blocked distinction; the ghost reads no colour at all. Nothing
about the squares changes and nothing is written into the engine: the ghost is a posed body draw
whose DATA arrives entirely in the frame packet. The engine hooks it depends on are TADR section
C's observers (`tagpu_datakeys.c`): the FBI reader (`0x42BF40`, `0x42BF97`) for `PreviewPieces=`,
and the COB length (`0x4B6BA0`) for the `Create()` mask.

- **The data.** `TAGPU_PK_BUILD`, a build-orders table the publisher copies out of the order
  pass's own game-thread snapshot — the same records `draw_build` draws the squares from, under
  the same gate (`s_build`, a non-zero `btype`, a resolved def) and the same lever arm state, so
  the ghost and the squares can differ only by the copy's age: the squares read the arena at
  present time, the ghost the copy the previous draw made of it, one presented frame at most.
  The cursor's unit type is `build_unit_id`, the header's copy of `BuildUnitID main+0x2CC4`
  ([VERIFIED LIVE](../exe-reverse-engineering.html): 78 = ARMMEX after a build-menu click), with the
  cursor's position the midpoint of `build_rect` and its gate `gather_cursor`'s own (mode 14, and
  the band bit or the mouse inside the rect the engine can NAME). Both ghosts bound their type by
  `udef_count` before the model table.
- **The draw.** The ghost pass lives in `tagpu_native.c` (the `fenced` file whose allowance the
  `MODEL_PTRS` template read stands on) and synthesises the piece list from the model tree —
  parents first, at rest, the same walk class the bake and `PK_PIECE.node` stand on — then bakes
  and poses: the rest pose **turned to the heading a built structure stands at** — per-piece
  translation by the bake's `restOff`, the whole of it rotated by `0x8000` about the root
  piece's own rest point, which is `posed_pose`'s output for a unit holding every piece at rest
  with that body turn (see the heading bullet below) — into render-thread scratch. One ghost = one
  bake lookup, one `glDrawArrays`, no pose arena. `fog=0` so it never fog-dims, like the square;
  no shadow, no nanoframe wire, no waterline. The material is keyed on the local player's logo colour
  (`local_logo`), so the ghost shows the player's own team colour — exactly what the built unit
  will look like. An earlier cut tinted
  it green/red (`uGhostTint`); the owner dropped the tint (2026-09-12): translucency alone, the
  existing `uAlpha` blend the cloak already rides.
- **THE PIECES THE FINISHED BUILDING HIDES, THE GHOST HIDES** (TADR section C, landing C1,
  [data keys](../tadr-port/data-keys.html)). A structure's COB `Create()` hides pieces the moment the
  unit exists — muzzle flares, a radar dish, a moho's rotary — and no script runs for a preview, so
  the ghost used to show them: 21 of the 126 stock structures hide a piece with faces. The mask is
  computed on the GAME thread by `tagpu_datakeys.c` through the rule in `tagpu_cobscan.c` (engine-free,
  the one the in-game wiki's models read too), cached during unpaused play, and published as
  `TAGPU_PK_GHOSTMASK`: type, piece count, template root, 256 bits, one row per type a ghost may
  draw this frame that hides anything, sorted by type (validated in `frame_valid`). What it hides:
  - **by default, `Create()`'s hides, read and not run** — from the entry point, through the
    opcodes whose length is certain (`tools/ta3do`'s `COB_PROLOGUE`), `HIDE` sets and `SHOW`
    clears a COB piece; the walk stops at the first opcode it cannot size
    (`tagpu_cobscan_hides`). Every read of the COB is a 32-bit offset from the block's base,
    bounded by the block's own length: the size `0x4B2450` checksums the block with
    (`0x4B6BA0(buf, size)`, the call that returns to `0x4B247C`), which an observer records. A
    script with no recorded length gets no row. The template's node names have no such length;
    they are read under the type bound and the level lifetime, as every reader of the tree is.
  - **matched to the model as the engine matches it**: `0x45A950` builds the primitives in
    `0x45AEC0`'s pre-order, then for COB piece *i* swaps in the first primitive at an index ≥ *i*
    whose node name `_stricmp`-matches. `tagpu_model_engine_order` (`tagpu_model3do.h`) lays the
    nodes out in that pre-order and `tagpu_cobscan_mask` runs the swap, so COB piece *k* names
    the node the engine will hide. On stock content it equals a by-name match: MEASURED
    2026-10-26 by `tagpu/ddraw/tests/cobscantest.py` over all 278 stock unit scripts, where the 90
    that hide a piece at `Create()` each carry distinct names up to their last hidden piece, each
    on exactly one node of the model, and the mask's nodes are `tools/ta3do`'s
    `hidden_at_create` for every one.
  - **with `PreviewPieces=` in the FBI**, every piece the list does not name (TADR's split and
    fold; a list that names no piece of the model is ignored and logged).

  The cursor's row rides every packet with a build on the cursor (mode `0x0E`, the packet's own
  `build_unit_id`), not the builds table's poll; a packet whose mask table was cut or overflowed
  (`TAGPU_PK_TRUNC_GHOSTMASK`) draws no ghost at all that frame, counted as `maskcut=` in the
  heartbeat, so no ghost draws with the pieces it should hide. The bits are in the ghost's own walk order, and **both threads call one walk,
  `tagpu_model_walk` (`tagpu_model3do.h`)**, so the order cannot drift. `ghost_one` applies a row
  only when its root and piece count equal the template it walked — two values compared, never
  followed — and a hidden piece takes `posed_pose`'s form: an all-zero matrix and visibility 0.
  A type with no row takes exactly the path it took before. **Measured** with
  `scenarios/ghost-mask.json` against the same tree without the mask, the cursor ghost at zoom 2:
  ARMLLT differs by 34 px, ARMAP 177, ARMHLT 360, CORINT 395, CORMOHO 1 121, each inside the hidden
  piece; CORDOOM (four hidden pieces, none with faces) and the no-row ARMSOLAR, ARMVP and CORFUS
  by 0; two shots of one build 0 in every case. `tools/datakeys_fixture.py`'s clones show
  `PreviewPieces=base` drawing the base alone.
- **The cache key — and the leak it closed.** The synthesised run walks the tree in *its* order,
  which is not the prim order a live unit's packet run carries, and the bake lays the VBO's
  per-vertex piece indices and `parent[]` out in run order. So the ghost's bake is keyed apart
  from the units' (`TAGPU_PBGEOM.ghost`, a `ghost` parameter on `tagpu_posebake_unit`): the first
  cut shared the slot, and a placed building — same root, same piece count — then drew its
  prim-ordered pose matrices against the ghost-ordered VBO, scrambling every part ("all of the
  model parts of building are translated in an incorrect manner"; the commander was immune
  because it baked before any ghost existed). Safe by construction: the two runs can never share
  an entry whatever the walk order.
- **The shader.** None. The ghost rides the posed program untouched — `uAlpha` is the cloak's
  blend, and the model's own colours pass through; the only per-unit field it adds is the
  ordinary `alpha`. (The 2026-09-12 tint cut removed `uGhost`/`uGhostTint` again, so
  `tagpu_native_unit_fs` is back to the shape it had before the feature.)
- **The piece walk's provenance, now that the ghost is a default.** `ghost_pieces` carries no
  per-node readability probe, on `aabb_walk`'s argument and by its lifetime (the level, under
  `tagpu_reclaim`'s teardown wrap). One difference from `aabb_walk` is worth stating rather than
  leaving to be found: `aabb_walk` walks the template of an INSTANTIATED unit, while
  `ghost_pieces` walks `mptrs[mid]` for any `mid` the caller's `mid < udef_count` bound admits —
  a def the player can select to build but whose model slot this level may never have
  instantiated. The slot is guaranteed non-NULL by that bound; that it is *walkable* is inferred
  from the engine's own unconditional walk at `0x4CB650`, not measured for a never-instantiated
  def. [Named by the 2026-09-14 landing review; no fault observed in 57 480 walks.]
- **A SITE THAT IS ALREADY A NANOFRAME GETS NO GHOST** [owner-reported and fixed 2026-09-21].
  A build order keeps its node for the whole of the construction, so the queue table carries the
  site the builder is *working on* exactly as it carries the ones it has not reached — and the
  ghost drew there too, standing a second, solid copy of the building inside the frame being
  built. The publisher now answers the question on the game thread, from the node's own target
  field: `OrderNode+0x16` (`N_TARGET`, the field `resolve_sprite` and `circle_centre` already
  read) is NULL while the site is empty ground and holds the created unit once construction
  begins, so `TAGPU_PK_BUILD.started` — the record's dead `pad`, repurposed — says which. It is
  an **exact link the engine itself made**, not a position match with a tolerance in it, so
  there is nothing to tune and nothing to race. **The SQUARE stays**: the engine draws its own
  over a frame under construction and the marker pass reproduces that; only the ghost drops.
  Counted as `built=` in the heartbeat. Measured on `one-unit`, three solars shift-queued: the
  one being nanolathed shows square and spray with no model inside it, the next one along shows
  square and ghost, and `built=` climbs at the frame rate while `queue=` does the same.
- **THE GHOST STANDS AT THE HEADING THE BUILDING WILL STAND AT — `0x8000`, NOT THE MODEL'S
  REST ONE** [owner-reported and fixed 2026-09-21]. Until this the ghost drew every piece at
  its rest offset with an identity rotation, which is heading `0x0000` — **half a turn from
  every structure the engine places**, and the owner saw it as the preview and the finished
  building disagreeing. The engine's spawn `0x485A40` writes the new unit's three rotation
  words as bank 0, pitch 0 and `unit+0x66 = 0x8000 - BuildAngle/2 + rand(BuildAngle)`
  ([exe-reverse-engineering](../exe-reverse-engineering.html) §"`0x485A40` — the spawn's rotation
  words"). The centre of that is `0x8000`, the default facing, and the ghost now takes it: one
  rotation about the root piece's rest point, which is what `posed_pose`'s chain collapses to
  when nothing but the base piece turns.
  **The spread is a residual this cannot close and does not pretend to.** The draw is taken
  from the shared sim seed `0x51FC88` at creation time, so it is neither knowable in advance
  (the number of intervening draws is unknown) nor ours to consume (drawing from it would
  desync a network game). 105 of the 278 unique unit FBIs in the reference install's archives
  set `BuildAngle`, so the residual is real and varies by type: absent on most mobile units,
  0 on the two forts, ±11.25° on ARMSOLAR and ARMESTOR (4096), ±22.5° on ARMMEX and ARMWIN
  (8192), ±90° on CORSOLAR and both LLTs (32768). The preview is the centre of that
  distribution; the building lands somewhere inside it.
  **Measured** with `scenarios/build-facing.json` — two ARMVPs, one at `facing 0` (heading
  `0x8000`) and one at `facing 180` (`0x0000`) — the ghost parked on clear ground at
  `alpha=1.0` (the token, so the silhouette is readable): the
  pre-fix ghost matches the `0x0000` reference — blue roof band at the bottom, ramps at the top
  — and the fixed ghost matches the `0x8000` one, band at the top and ramps at the bottom.
  Same instance, same scenario, same camera, the two DLLs swapped under `--keep-dll`.
- **The lever.** `tagpu_ghost.on` (tokens: `alpha=<f>`, default 0.40), re-read on the pass's own
  30-frame poll; the armed line and the `ghost: curs= queue= drawn= built= nobake= trunc=
  masked= maskmiss= maskcut= alpha=` heartbeat log only on change / every 300 frames. `nobake`,
  `trunc`, `maskmiss` and `maskcut` must stay 0; `built=` and `masked=` are counts, not alarms —
  `masked=` is the ghosts drawn with a mask row. **It needs
  `tagpu_native.on`** — the ghost draws through the unit pass's view and program — and says so:
  armed without it the log reads `ghost: off — needs tagpu_native.on (it draws through the unit
  pass)` and the pass declines. **Since 2026-09-14 it IS a play default** and carries `needs
  tagpu_native.on` in `tagpu_opt.c`'s table, so the table withholds the default when the unit
  pass is off. The runtime refusal above stays, and is not redundant: `needs` governs the
  DEFAULT, not the lever, so a hand-written `tagpu_ghost.on` file arms the pass whatever the
  table says. Turn it off with `tagpu_ghost.off`, or with `tagpu_defaults.off` for the whole
  table.
- **The review's fourteen findings (2026-09-12, xhigh, one reviewer) — what changed.** The pass
  now checks its two prerequisites every frame (the posed program live; `s_pv` THIS frame's,
  stamped when the unit pass fills it) instead of assuming them; it re-binds the posed program's
  seven texture units itself (`ghost_bind_textures`) because `tagpu_fx_render` rebinds 0/1/2/4/5/6
  for its own program and restores nothing; it draws with **depth writes off** so two ghosts at
  one site blend instead of the second being culled by the first's depth; it culls both ghost
  kinds against the widest-zoom rect the unit gather uses; the cursor's `build_rect` is bounded
  and projected exactly as `gather_cursor` bounds it (so a rect the square refuses draws no
  ghost, and the midpoint adds cannot overflow); the anchor halves the altitude with the
  **square's** truncating `>> 1`, not the unit pass's float, so the ghost sits ON its square; a
  model the piece walk cannot hold is **refused and counted** (`trunc`) rather than drawn as half
  a building; the queue table is validated like every other packet table (`frame_valid`); and
  `tagpu_order_copy_builds` is gated on the order pass's own arm state, so disarming it takes the
  queue ghosts with the squares instead of leaving the last publication frozen on screen.
  **Measured in game**: the cursor ghost centred on its square (bbox 975..1019 against square
  973..1021, centroid x 996 vs 997); queue ghosts drawing from a real record (`bt=78 bdef=1`
  through the packet); `posed=2/361tri` with **no `q=` suffix** while thousands of ghosts drew
  (the false "queued units not drawn" heartbeat this fixes); `trunc=0`.
  **Not verified in game**: the depth-overlap and disarm checks — the cursor and queue ghosts are
  not co-located (their anchors come from `build_rect` and the order node, ~18 px apart on the
  tested site) and every candidate site was animating (a nanoframe under construction), which is
  a noise floor of ~2.8 k px against a ~950 px ghost. Both changes are read-verified against the
  code path they guard.
- **Latent, not fixed by the rebind alone.** With Classic++ on (the play default) a body fragment
  takes its colour from the rgb atlas on **unit 8**, which fx never touches, and samples unit 0
  only for the texture's colour-key discard; measured this session, unbinding units 0/1/2 changes
  the ghost not at all in that configuration. The clobbering bit when the LUT/palette path was
  live (`uLit == 0`, Classic++ off) and, for keyed textures, in the discard test — which is what
  the rebind removes. *That path is deleted since G20d: both presets sample the RGBA8 base atlas
  (§2.88).*
- **Verified in game** (one-unit fixture, play defaults): the mex ghost at the placement cursor
  and at the queued site rects, diffed against `ghost.on=off` and found only at the ghost's own
  footprint; after the cache-key fix, a placed mex renders pixel-identical with the ghost armed
  and disarmed (mean diff 6.0 vs the 6.7 off/off baseline).
