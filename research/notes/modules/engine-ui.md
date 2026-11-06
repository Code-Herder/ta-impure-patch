# The engine's UI

What the engine's UI draws, the layer that observed it, and the save-under free.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.3e The engine's UI, observed — and the layer that drew it, deleted (`tagpu_gui_hook.c`, `gui.on`, Phase E G15a + G15b + G15d + G15e + G17a–e)

> **[SUPERSEDED THE SAME DAY: the drawing half came back on Vulkan without the engine's half —
> `tagpu_gui_surf.c` drains the op queue into the twins and `tagpu_vk_gui.c` draws them and the
> sharp layer (cursor, minimap) over the world; `LAY_FS` declares no sampler for TA's surface.
> `tagpu_gui.h`'s opening block is the current description.]** The callout as written:
>
> **READ THIS FIRST — THE DRAWING HALF OF THIS SECTION NO LONGER EXISTS [§2.81, 2026-09-20].**
> The clean cut deleted `tagpu_gui_surf.c`, `tagpu_vk_gui.c`, the twins, the sharp layer, the
> composite and their seven shader programs. Everything below about a twin, a mirror, a layer, a
> sharp ramp or a minimap base is **history**, not current behaviour, and the figures in it were
> real when they were taken. What still runs is the OBSERVER half — the detours, the op stream and
> the census — which survived because capture is not compositing and because `before_flip` is the
> only host of `tagpu_triggers_frame`. The hook table below is current; the prose around it is
> not. The engine knowledge is [exe-reverse-engineering](../exe-reverse-engineering.html) and
> [gui-renderer](../gui-renderer.html).

**Phase 2 was complete.** G17e (2026-09-09) made the **minimap** ours at `k > 1`: the base from the
TNT's own 252-px picture instead of the 126-px box the engine fits it into, and the fog, the unit
dots, the radar arcs and `DrawPoint`'s points all taken from the engine's own pixels by masking
`+0x142DB` and `+0x142DF` against `+0x142E3` — so the visibility decision never leaves the engine.
2 084 distinct colours against the engine's 532 at `k = 1.5`, and 2 differing pixels of 13 356 on
a 99.6 % fogged map. G17d (2026-09-09) made text a **string op**: `PK_STRING` carries the string,
the font and `0x4CCF60`'s three colour bytes, and the render thread stamps TA's own glyphs into the
twin from a **per-font glyph cache** — the string-keyed atlas the marker path uses is wrong for a
UI whose text is a clock and a metal readout. 44 % less arena traffic where text is redrawn, and
two 120-stop walks with `miss=0`. G17a (2026-09-09) added the seam: a 4-tap sharp-bilinear ramp on the 1x mirror
run after the palette lookup, a device-resolution RGBA8 **sharp layer** above it, and `k` read off
the frame. G17b (2026-09-09) made `k != 1` live and added device-space click injection. **G17c
(2026-09-09) put the cursor in the sharp layer** — ours at 1x device pixels at every `k`, from the
client point rather than the engine's logical grid, with the engine's own erased by the UI layer
and the world composite together ([GL UI renderer](../gui-renderer.html) §15–§17). Still no engine
patch in phase 2 and no new engine-state write.

**AND SINCE 2026-09-22 THERE ARE FOUR CALL-SITE REDIRECTS BESIDE THE 17 LEAVES — the world
phase.** They carry no observer and record no op: each is the 5-byte `call rel32` at a
`DrawGameScreen` site repointed at a thunk of three instructions — `mov byte [s_world], v` then a
tail `jmp` to the engine's own function — so the engine's arguments, its stack and its return are
untouched and redirecting the SITE composes with whatever is detoured on the CALLEE. They bracket
the two spans in which `DrawGameScreen` paints the world, and `op_add` stamps every op recorded
inside one; `publish` then refuses it, because world content may not cross into a UI replay
whatever leaf carried it. Byte-matched all-or-nothing at attach, installed with the leaves and
reported as `worldphase=` on the `gui: ARMED` line; `tagpu_worldphase.off` is the A/B lever and a
byte mismatch arms nothing and drops nothing.

| site | calls | what it opens or closes |
|---|---|---|
| `0x468DB0` | `0x483FA0` terrain | span 1 **opens** |
| `0x468E3A` | `0x4C69C0` clip reset | span 1 **closes** — the METAL/ENERGY block and the minimap follow, and are UI |
| `0x469849` | `0x471F90(ctx, 0)` particle layer 0 | span 2 **opens** |
| `0x469F36` | `0x435100` a mode query (`__fastcall`, ECX) | span 2 **closes** — the HUD extras tail follows, and is UI |

Both closers are branch joins reached on every call of `DrawGameScreen`, so the flag cannot leave
the function. Why these four and not the obvious single bracket, the branch evidence and the
subtree call lists: the [engine map](../exe-reverse-engineering.html), *The TWO WORLD SPANS*, and
§2.81's `*own` subsection for what it cost and what it bought.

Nothing here changes what the engine draws. Every leaf site is an **observer detour**
(`tagpu_detour_observe`): the original runs unchanged, we read its arguments on the way in and,
for the allocator, its result on the way out; every register and EFLAGS are saved around both
calls (`pushfd/pushad … popad/popfd`, since the 2026-09-07 review). Installed once at DllMain when `tagpu_gui.on`
exists, byte-matched, all-or-nothing — 17 sites. **One site was already owned**: `fxown` holds
`CopyGafToContext 0x4B7F90`, so the observer **chains** onto fxown's stub (the shared detour code
now records every landed stub and hooks the earlier stub's copy of the stolen bytes; `own the
draw` §"chaining") rather than overwriting its jmp — fxown's skip still wins, and the observer
sees only the blits that really draw. Full argument lists, boxes and evidence: the engine map's
"The UI surfaces and their writers".

| VA | What it is | Stolen | Observer records |
|---|---|---|---|
| `0x4C63A0` | `FlipOffscreenToPrimary` — the engine's "this frame is complete"; the census runs here, **and since the vulkan-only plan's landing 10c so does the whole on-demand trigger family** | 6 | the frame marker; diffs `*(globals+0xBC)` and every surface an op named. **Also `tagpu_triggers_frame`** (`tagpu_trigger.h`): peek, the weapon dump, the GUI snapshot, the unit/feature catalogues, scenario detection, — since 10c-2 — `tagpu_input.c`'s token half, and — since 11-2 — the engine-surface screenshot, which is the one member that answers a pass LATE: it services the previous pass's arm before polling for a new one, because at the flip's entry the primary still holds the frame the previous flip presented (§2.64). It runs from `before_flip`, above that function's three early returns, behind a **16 ms QPC gate** with its own counter: these throttle on `frame_counter % 5` (`% 15` for peek) written against the ~60/s *present* rate, and the shell flips **thousands of times a second** — `CENSUS_MS`'s comment beside it reads ~5 000 flips/s (measured 2026-09-07) with the op census at ~12 000 *ops*/s on MAINMENU, while §5352 and the GUI-renderer page say ~12 000 *flips*/s; **the two readings are not reconciled** and the gate does not depend on which is right, being a bound rather than a rate assumption. This is the only reason any `tacli` verb works on `renderer=gdi`, which reaches `tagpu_overlay_draw` never — **and therefore the observer installs whenever the flip's bytes match, NOT behind `tagpu_gui.on`**. For one commit it was behind that trigger, which `tagpu_defaults.off` (written by a bare `tacli launch`) takes away, so an ordinary instance had every `tacli` verb and all input injection silently dead **on every renderer**, with no line in the log because the early return was above the only one. The landing review of 10c-2 caught it. `tagpu_gui.on` now gates the UI layer, the census and the leaves alone; `tacli gui remove` removes those and no longer removes the ability to drive the instance. The one trigger NOT here is `tagpu_input_eye_frame`, the camera hold: it dereferences `f->packet` and this frame has none. **A movie frame is recorded here too**: a flip returning to `0x47C455`, the one flip of the movie's frame routine `0x47C3A0`, records the box `SmackDoFrame` filled as `OP_MOVIE` and publishes it in the same call, past the census cadence, as a `PK_MOVIE` with the decoded bytes ([GL UI renderer](../gui-renderer.html) §25) |
| `0x4C67C0` | the cursor draw **inside** the flip (`stdcall(globals, surface)`, `ret 8`), the shell's publish point since landing 6 — **its own observer, in `tagpu_packet_pub.c`**, not this census. The function is UNPATCHED and now unpatchable by us: `tagpu_cursown.c` used to skip the `call` at `0x4C687D` that blits the sprite, and **the whole module was deleted by the clean cut** (§2.81) because the composite that replaced the cursor is gone. The observer, the background save at `0x4C6862` and the `+0x1B6/+0x1BA` writes always ran and still do; the blit now runs as well, which is what keeps the cursor in the reference frame. (A leaf on the whole function was tried on 2026-09-13 and withdrawn — GUI renderer §24.0) | 11 | the drawn cursor's record `+0x1B2`, and the three early-out words. Publishes a header-only `in_game = 0` packet carrying the record as `cur_rec` when the three words hold, and `cur_rec = 0` when they do not (the position `+0x1B6/+0x1BA` and the `cursor_live` flag are not published since 2026-09-23) — gated on **two** tests, `s_retDepth == 0` **and** `!s_levelOpen`. `s_retDepth == 0` alone is NOT the complement of the in-play gate: `0x495E66` calls `DrawGameScreen` and returns to `0x495E6B`, not the in-play `0x4969D2`, so a screenshot draw is in-play with `s_retDepth == 0`; `!s_levelOpen` is what keeps this channel out of a level. This row said "i.e. not inside an in-play draw" until the 2026-09-14 review — the code has always had both tests |
| `0x4C25E0` | the body of the engine's **mouse thread** (`0x4C2990` is its entry, started by `_beginthread` at `0x4C2A9A` — which is why no `call 0x4C2990` exists); `stdcall(mouseObj)`, `ret 4`. Unpatched as a function, and since the clean cut (§2.81) unpatched entirely: its cursor blit `0x4C2732` was one of the four `tagpu_cursown.c` skips and that module is deleted | 8 | nothing — no observer, it is not a channel site. It writes `+0x196/+0x19A` and `+0x1B6/+0x1BA`, the latter as position **minus the hotspot**, exactly as `0x4C67C0` does [CORRECTED 2026-09-14: this row claimed the opposite and called it a fingerprint] |
| `0x4B7F90` | `CopyGafToContext(ctx, frame, x, y)` — **chained onto fxown's stub** | 6 | a sprite box at `(x−HotX, y−HotY)`, clipped |
| `0x4B8500`, `0x4B8310` | the shaded blit, and DrawText's alternate blit — which is **not** the same shape: five args, gated on `globals+0xF0` bit 7, and two arms: a raw frame BLENDS with the destination, an RLE frame is REMAPPED through row `a5` of the lighten table and reads no destination ([engine map](../exe-reverse-engineering.html) "The leaves"). `OP_GAFA` and the raw arm of `OP_GAFB` are published as box bytes, which the drain drops, so **they do not reach the screen on the Vulkan lane**. The RLE arm of `OP_GAFB` does: `gafb_capture` remaps the plane at observe time and it crosses as an ordinary `PK_SPRITE` (2026-09-23). No op is recorded when bit 7 is clear, because the engine draws nothing then | 6 | the sprite box, as for `0x4B7F90` |
| `0x4C6D20` | the rect copy `(ctx, src, srect, drect)` — the listbox's and textfield's background restore | 7 | **an `OP_COPY`** — `drect`'s top-left by `srect`'s extent, clamped to the destination, with the source's top-left — whenever `srect` lies inside the source, which publishes as `PK_COPY`; otherwise the box (`OP_GAFD`). The engine takes the size from `srect` alone ([engine map](../exe-reverse-engineering.html) "The leaves") |
| `0x4C7580` | the textured-**quad** stamp `(ctx, src, xy[8], uv[8])`, FOUR vertices — the option screens' wide backdrop, the in-game badge, SELMAP's map preview | 5 | the vertices' bounding box, over all four. An axis-aligned, unclipped stamp of a single frame also carries its resample (`scale_capture`) and publishes as **`PK_PLANE`**, a box of indices: the span `0x4C7310` copies every texel, key colour included, so there is nothing to key |
| `0x4CCF60` | the glyph blitter, cdecl 9 args | 6 | the string's box from the font's width table, the string's bytes copied into a window scratch, and **since G19f-8 the font itself**: the slot id, `font[0]`/`font[2]`, and a glyph record for every code of this string the font has not sent yet (`text_capture`). `publish` then dereferences no font at all — the read happens one instruction before the engine's own, which is the whole of the lifetime argument |
| `0x4BE950`, `0x4BF6F0`, `0x4BF8C0`, `0x4BF7B0`, `0x4BF4D0`, `0x4BFE10` | line, bar, hollow rect, focus rect, box shader, grey remap | 8/7/6/7/7/7 | the rect, clipped — **except the focus rectangle, which records FOUR ops since landing 8d (2026-09-21), one per edge in the engine's own order (top, right, bottom, left), each clipped on its own as `0x4BEA20` clips them.** The box is not what `0x4BF7B0` draws: its four edges go through four separate `0x4BEC70` calls, so a single clipped box would close a figure the engine left open, and the four shared corners are tinted twice. It also refuses a shade level outside the lighten table's 32 rows, which nothing in the engine bounds. **The box shader `0x4BF4D0` records its box and its level (`OP_FRAME`), clamped as the engine clamps it**, and publishes as `PK_TINT` whose row is that level's in `PK_SHADE` — a list's selected row, a disabled row, the modal dim; a level whose row is 0 publishes the box instead ([GUI renderer](../gui-renderer.html) §`PK_TINT`). **The grey remap `0x4BFE10` records its box (`OP_GREY`)** and publishes as `PK_TINT` through the grey table's row: the first half of a greyed gadget, which the box shader at −20 then darkens ([GUI renderer](../gui-renderer.html) §26) |
| `0x4C6890` | `SurfaceFill(surface, colour)` | 7 | the whole surface |
| `0x4C6B70` | surface → surface `(dst, src, x, y)` — the GUI panel reaching the frame | 8 | the source's box at `(x−originX, y−originY)`, clipped |
| `0x4C69F0` | `SurfaceCreateNamed(tag, w, h)` — return hijacked | 6 | registers the surface, seeds its copy so its build is diffed |
| `0x4C6AC0` | `SurfaceFree(surface)` | 6 | retires it: the entry stays, marked dying, and an `OP_FREE` holds the free's place in the window, so a copy out of it recorded earlier still publishes twin to twin (§2.104) |
| `0x4D85A0` | `MEM_Free(block)` — the allocator's own free, **not a pixel writer** | 5 | retires every tracked surface inside the block (G18-8): each entry whose base lies in `[block, block + MEM_Size(block))`, the size asked inside the allocator's own critical section (`surf_dies_with`, `mem_block_size`) — a `0x4C69F0` surface's pixels sit at `block+0x30`, a frame's at `block+0x18` or its second plane — and this is the only way an engine allocation dies — on the game thread in place, as `SurfaceFree` does (§2.104); a free on another thread is asked of `live_hit`, a mirror of the tracked bases that nothing moves, and only one that names a tracked surface is queued and dropped at the next flip ([GUI renderer](../gui-renderer.html) §31: the loader thread's 33 191 frees per load used to overrun the queue and retire the whole table) |
| `0x4A81E0` | `GUI_StageUpdateDraw(gi, flags)` | 10 | a build/redraw event for the log |

**AND ONE OBSERVER THAT IS NOT A LEAF: the list wheel on the GUI pump `0x4A9FD0`** (stolen 5,
`83 EC 34 53 55`; an entry hook only). `tagpu_gui_init` installs it whether or not
`tagpu_gui.on` exists — it does not depend on the op capture and works on every renderer —
byte-matched on its own, logged as `gui: list wheel ARMED`, and left out by
`tagpu_listwheel.off` at launch. The window procedure's `tagpu_gui_wheel`, called at both wheel
doors (`wndproc.c`, the shield's `to_game`) after `tagpu_zoom_wheel` declines a notch, puts the
notch's delta and point on a single-producer single-consumer ring — slot written, then the head
published with an interlocked store — with the engine event ring's fill at that moment, read from
the graphics globals `*(0x51FBD0)` on the game thread (the window's thread; a notch from any other
thread is dropped and counted). The observer drains the ring
on the game thread and scrolls the list under each point: it writes the list's `+0xBC`, calls
the engine's `0x4A1B40`, and moves the gadgets bound to the list as `0x4A2BE0`'s list and slider
arms would (§2.5). A notch is applied only once the engine has taken every click queued before
it off its event ring, so it never overtakes one. Behaviour and measurement:
[GUI gadgets](../gui-gadgets.html) §2.4.1.

**Two writers of the minimap composite are missing from this table, and what saves them is an
accident** [VERIFIED 2026-09-08]: the radar coverage arcs `0x4C0070` and `DrawPoint 0x4BEE60`
write `main+0x142DB` and nothing observes them. They land in the twin regardless, because
`0x466DC0` copies the base in first and that copy's source `main+0x142DF` — written only by
`0x466C20`'s direct byte writes — is never seeded, so the copy publishes as a **pixel op carrying
the destination's final bytes**, arcs included. **Seed copy sources on demand and the arcs
disappear**: the copy would become a real twin→twin `PK_COPY`. The census cannot catch it either
way, since those pixels sit inside the copy's box ([GL UI renderer](../gui-renderer.html) §7).

Tokens in `tagpu_gui.on`: `strict` is gone — it named the GL composite's engine-frame fallback,
and nothing on the Vulkan lane read it (2026-09-23). `off` (the detours stay installed for the
next launch, nothing is published or drawn — the live A/B lever), `norestore` (G15e: the layer without Classic++ art),
`sharptest` (G17a: the sharp layer filled with a known pattern — the harness's mode too),
`nocursor` (G17c: phase 1's cursor, the engine's own — the A/B against ours), `cursorscale=N`
(G17c: our cursor's size in device pixels per art pixel, default 1, clamped 0.25–8),
`nostring` (G17d: text stays a box of captured pixels — the A/B, and **read at ATTACH like
`census`/`log`/`pgm`/`trace`, so it must be armed before the launch**; only the tokens the *surf*
module owns follow the file live), `nominimap` (G17e: the engine's minimap back) and `mmbase`
(G17e: ours forced on at `k = 1` too, where it is otherwise off — the harness's A/B),
`census` (the G15a diff), `log` (a census line per 50
censuses and on any residual), `pgm` (`tagpu_gui_census.trigger` → `tagpu_gui_census.pgm`, the
accumulated unexplained mask), `trace` (the ops intersecting a residual, the first blits after a
build, every allocation with its tag), `probe=x,y` (with `trace`: every published op touching
that pixel of the presented surface), `key=N`. **MEASURED** 0 unexplained of 3 710 035 changed
pixels across the inventory (engine map, "What the census measured").

**The layer (G15b).** Nothing is patched beyond the observers above; the drawing is a second
half on the render thread, and the two halves meet only in a lock-free SPSC queue
(`tagpu_gui_int.h`: 65 536 ops and a 16 MB byte arena).

- *Game thread, inside the flip observer, at the census cadence (≤ 1 per 5 ms):* the ring of
  ops since the last publish becomes queue ops. A **seed** (the surface's bytes, whole) for
  every surface first seen since the last reset; a **sprite** for a plain keyed `0x4B7F90`
  blit of a frame ≤ 512 px with no sub-frames — the frame identity `(header, pixel pointer)`
  plus, on first sight, its pixels decoded here by `tagpu_gaf_decode` (the shell frees a
  popped screen's art under the render thread, so the pointer must not be read there); a
  twin-to-twin **copy** for `0x4C6B70` when the source is twinned; a **clear** of the true
  viewport rect at every flip after which `terrown`'s fill sequence advanced (the terrain skip's
  key fill is the engine's per-frame erase, mirrored); and for everything else — text, lines,
  rects, fills, the descriptor blit, the textured triangles, the shaded and sub-frame GAF
  variants, a copy from an untwinned source — **pixels**: the box's bytes as they stand at
  publish time, so a pixel op is the final state of its box and the twin converges on the
  engine's surface whatever order the writers ran in. **Identical ops within one batch collapse
  to their last occurrence** (the shell redraws every gadget on every flip — ~41 ops at ~12 000
  flips/s on `MAINMENU` — and without this the queue overflowed into a reseed storm).
  Excluded, on the return address: the unit composite blit, the cursor code, the flip's own
  blits (engine map, "What the twin layer excludes, tests and reads"). Three rules from the
  landing review: a sprite's identity is the frame's addresses **plus a hash of its whole
  plane** (a popped screen's art is freed and the heap reuses the addresses); the batch
  dedup **never moves a write past a copy that read it** — an earlier duplicate is dropped only
  when no `0x4C6B70` reading its surface lies between the two, or one follows the survivor
  (a per-surface epoch bumped by every copy was tried first and re-created the shell's reseed
  storm, since the shell copies its panel to the frame on every flip); and `SurfaceFree` **zeroes the
  ring's ops on the freed base**, so a surface re-allocated over the same bytes before the next
  census is never diffed or replayed against an old box.
- *Render thread, inside `tagpu_overlay_draw` after `tagpu_native_frame`:* poll the trigger
  (500 ms), drain the queue (20 000 ops per present at most), then draw. Every seeded surface
  has a **twin**: an `RG8` texture its size (R = the palette index, G = coverage) behind an FBO,
  1:1 and `NEAREST`; sprites are quads from a `TAGPU_GAFATLAS` of the UI frames (2048², `pad 0
  align 0 mip 0`, colour key discarded in the fragment), copies are quads sampling the source
  twin, pixels and seeds are `glTexSubImage2D`, clears are scissored `glClear`s to coverage 0.
  **The seam is one draw**: the presented surface's twin over the whole frame, into the
  overlay's target FBO with blending and depth off, `discard` where coverage is 0 — so the
  native composite's key rule beneath is unchanged and the engine's pixels remain the
  fallback wherever a twin has nothing. The index is resolved through **the palette the frame
  is presented with**, not `main+0x143A7` (G15d, below). The cursor's rect
  (`*(0x51FBD0)+0x1B2/+0x1B6/+0x1BA`) is left to the engine's frame. `strict` paints a miss
  magenta instead of falling back, outside the viewport or on a non-key pixel inside it.
- *The seam, since G17a (2026-09-09):* that one draw now composites **three** layers, top down —
  the **sharp layer** (one `RGBA8` texture at the *device* resolution, row 0 the viewport's top,
  cleared every present, taken where its alpha says it has coverage), the **1× mirror scaled by a
  sharp-bilinear ramp**, then the engine's frame. The ramp is a 4-tap run **after** the palette
  lookup — interpolating indices is meaningless — with each tap premultiplied by its own coverage
  so an uncovered texel contributes nothing rather than dragging index 0 in from the key fill, and
  the coverage thresholded at 0.5 after the blend. Its width is one *device* pixel, so at `k = 1`
  it is one source texel and the blend collapses to the single tap `texelFetch` would have taken —
  identical after 8-bit quantisation, with three decades of margin, which is G17a's whole gate.
  `k` is `vp_w / twin_w` read off the frame; the *engine* running at `window / k` is G17b's, but
  **`k` is not 1 on every phase-1 path**: `resizable` defaults TRUE and `maintas` fits the
  viewport to the client, so any window dragged off the game resolution is already fractional.
  At *integer* `k` the ramp is exactly nearest (measured at `k = 2`). The sharp
  layer is **empty** until the cursor (G17c) and the string op (G17d) fill it, so the token
  `sharptest` fills it with a known pattern — that is how an empty layer is testable at all.
  Measured: the parity fixture's frames byte-identical to `main`'s with Classic++ off and on, the
  120-stop `strict` walk unmoved, `fps=60.0` ([GL UI renderer](../gui-renderer.html) §15).
- *Classic++ (G15e, 2026-09-08):* every surface may carry a **colour twin** — `GL_RGBA8`, the
  same size, `COLOR_ATTACHMENT1` of the twin's own FBO — and the sprite and copy programs are
  **MRT**, so one draw writes the index and the restored colour together. A sprite takes the UI
  atlas's restored texel where the atlas has one (alpha 1); **a copy carries both channels**,
  which is what makes restored art survive the `panel+0xBC` → frame blit, and a copy from a
  source with no colour twin writes zero and so invalidates the destination there. Seeds and
  pixel ops carry indices only and drop the colour of their box. The layer chooses **per texel**:
  restored where alpha is 1, the live palette elsewhere. **The palette-validity rule**: the
  restore job snapshots the palette into a texture of its own, so its colours hold only while
  that is still the palette the frame is *presented* with — compared every frame, colour ignored
  while they differ, and after 30 still frames the job is rebuilt against the new palette and
  every colour twin invalidated. **The UI also steps the restorer when nothing else does**: `tagpu_rglsl_step`'s
  only other caller is the native pass, which returns early with no unit array — in the shell and
  with the world passes disarmed — so without this the UI atlas's queue is never drained and the
  shell stays indexed for ever (found by the landing review; `tagpu_rglsl_calls()` compared across
  presents means the budget is still sliced once per frame). The atlas restores at **priority 4**
  (`MAX_JOBS` is 6 since this landing; 0–3 are terrain, features, effects, 3DO units, and 5 is
  the UI's picture store since 2026-09-26 — every slot taken) and skips
  frames under **12×12**
  (`restoreMinEdge`, G15-0's verdict). Trigger token **`norestore`** is the A/B. Heartbeat gains
  `cpp= col=<made>/<live> colvalid= rearms= rgb=`. **Colour reaches a twin only through a sprite
  op, so seeded art stays indexed until the engine redraws it** *[since 2026-09-26 a backdrop's
  `PK_ASSET` and a stamp's `PK_PLANE` take the picture store's restored colour, a tint shades the
  colour instead of dropping it, and a colour repaint re-seeds the post-game snapshot —
  [GUI renderer](../gui-renderer.html) §27]* — entering a game, the panel is
  seeded and indexed until a repaint. Measured: `fps=60.0`, 37 415 of 45 056 px of the menu's
  panel rect against `norestore`, **7 963 px of the 640×480 shell frame**, and `+gamma 15` → 235
  entries differ, one re-arm, colour valid again ([GL UI renderer](../gui-renderer.html) §14).
  **The gate closed 2026-09-09 with the Q2 diff** (`uiwalk.py --restore` fills the atlas,
  `tascene uidiff` holds the dumped twin to the same restorer run offline on the dump's own
  cells): shell and game, 1024×768 and 1920×1080, **far band max 1 level on 0.0007–0.0017 % of
  bytes, 0 unmatched, alpha and the replicated border exact**, and a near band bounded *tighter*
  than the feature twin's the owner already accepted — so the `uirestore` exclude list stays as
  G15-0 ruled it. Two facts it forced into the record: **`e->wrap` is decided once, when a frame
  is first atlased, and a frame first seen while the palette is uniform keeps a tileability flag
  the settled palette would not produce** (one entry of 34 in one shell run; restored the wrong
  way it is 10 levels off over half its texels — the fix is to recompute it on the re-arm, not
  taken here); and **the presented palette was `min(255, (int)(entry × 1.125))` on the instances
  measured**, so `paldiff=235` was the ordinary reading and every pass still on `main+0x143A7`
  drew the world ~11 % darker than the engine presents its own pixels. **The attribution to "the
  template wine prefix carries `Gamma = 15`" is withdrawn** ([GL UI renderer](../gui-renderer.html)
  §15, measured 2026-09-09): the template and all 58 instance prefixes are **one inode**, wine
  rewrites it in place at launch, it now reads **12**, and instances launched under it present
  `paldiff=0` — no seam. The mechanism is unchanged; the *value* is mutable (under tacli each
  instance's own, in its registry store, §2.92), so read `paldiff=` rather than assuming it.
- *Fresh starts:* the trigger reappearing, a queue or arena overflow, a sprite whose bytes never arrived, a copy
  from a source with no twin, and the consumer coming back from a stall (below) all raise
  `reseed`; the next publish sends a **reset** and seeds every surface again from the
  engine's bytes. Nothing is reconstructed from history. **Since G15d every reset is logged
  with its reason** (`gui: reset #n: arm | queue-full | arena-full | box-outside-surface |
  lost-sprite | atlas-full | untwinned-copy | stall-over | string-empty | level-changed`, with the
  queue and arena occupancy), so the heartbeat's `resets=` is never a bare count.
- *The consumer can die, or crawl (G15d):* cnc-ddraw stops its render thread inside every
  `SetDisplayMode` and starts a new one, and on the way out of a game the
  old thread presents only every few hundred ms while the game thread is in the exit path —
  while the shell already flips ~5 000 times a second and the game frame publishes ~150 KB of
  box bytes per 5 ms cadence, so the 16 MB arena is half a second of backlog. The publisher
  therefore **drops its batch** when the tail has not moved for 250 ms with work queued, or
  when its last publish found no room in the ring or the arena (`stalls=` counts the
  episodes), and publishes again — one reset, every surface re-seeded — once the consumer has
  caught up. A backlog that still fits is not a stall: a half-arena rule looped on SKIRMISH
  on the Windows test box, 147 reseeds a minute ([gui-renderer §29](../gui-renderer.html)). (The drain also skipped every op published before a GL context change up to the
  producer's next reset, `skipped=`; with no GL context that path went, §2.85.) MEASURED
  2026-09-07: before, every game → shell switch cost 38 `arena-full` overflows, 39 resets and
  705 lost sprites; after, one reset (`stall-over`), no overflow, none lost, and the game's own
  OFFSCREEN — freed to the heap by `MEM_Free` at `0x491AB8`, not through `SurfaceFree`, and
  re-created 640×480 on the same base — no longer leaves a 1024-wide box in the ring for the
  next publish to trip on (`surf_get` forgets a base's ops on a same-base size change).
  **Since G18-8 that surface is retired at the free itself** rather than at the next
  `0x4C69F0("OFFSCREEN")`: the observer on `MEM_Free 0x4D85A0` is the surface's destructor, so
  no entry in the table can outlive its block whatever path freed it. That is the whole safety
  argument for `pub_surface_bytes` reading engine memory at the flip — the range test on the
  pointer was never one. MEASURED 2026-09-12: the two frees the old rule left standing are the
  shell's 640×480 offscreen at the mode switch (`0x498398`) and the game's at leave-game
  (`0x491ABD`), both on the game thread.
- *The palette (G15d):* the twin resolves through **the palette the engine's frame is presented
  with — cnc-ddraw's `g_ddraw.primary->palette->data_rgb`, what the engine's `SetEntries`
  stored — not `main+0x143A7`**. The engine scales every palette it sets by the Gamma option on
  its way to DirectDraw (`0x4BA200`, `SetGamma 0x4BA590`; engine map, "The palette the screen is
  presented with") and never scales its own table, so at Gamma ≠ 12, or after `+gamma N`, the
  two differ by the factor (the world takes the unscaled table and applies the factor once, at
  its composite — §2.3f). The read is
  taken under the fork's `g_ddraw.cs` (the game thread NULLs the primary inside it); the
  engine's table is the fallback until a primary exists. The heartbeat carries `palchg=` (uploads
  — a fade is a run of them) and `paldiff=n@i` (entries where the presented palette and
  `+0x143A7` disagree, and the first): 0 in game, 1 (index 9) in the shell, **235 (from index 1)**
  at `+gamma 15` — the count `uiwalk.py` records and this page had as 255 until 2026-09-09.
  **Since 2026-09-09 this is not the UI layer's own code**: the resolution moved into
  `tagpu_pal.c` and serves the world's passes too (§2.3f), which is where the same-frame
  disagreement it measured actually was.

**MEASURED 2026-09-07** (`tools/uiwalk.py --layer`, `strict`, every world pass armed): **0
differing pixels outside the viewport and 0 holes on every in-game stop** — `ARMMAIN2`,
`ARMCOM1`, its second page, `ARMOPT`, `PREFS`, `VISUALRT`, back out, chat, the F4 popup — at
**1024×768 and at 1920×1080**; every shell stop 0 except `MAINMENU`, whose ~185 differing
pixels are its sparkle animation between the two shots (the diff is single scattered pixels
in the sky, none on a gadget). Resets 3 per run (the arm, the shell→game context switch, the
game's mode switch), overflows 0, the atlas at 123 of 4 096 entries after the whole
inventory. Frame rates and the parity md5 with the trigger absent: [GL UI renderer](../gui-renderer.html) §10.

**MEASURED 2026-09-07, G15c** (the same walk, `--side` and nineteen more in-game stops, the
compare extended to every non-key engine pixel *inside* the viewport, each GL shot bracketed by
two surface shots): **ARM and CORE, at 1024×768 and 1920×1080, 32 of 32 in-game stops at 0
differing pixels outside the viewport, 0 inside it and 0 holes** — the `+clock` and `+bps`
strings, the hold-SPACE box, `PREFS`, `VISUALRT`, the F4 box and the chat over the world at 1×,
0.5× and 2×, the minimap's dot and view box after a move and a scroll. No hook, no field and no
GL object changed for it; the census on CORE explains 1 717 044 of 1 717 044 changed pixels.
[GL UI renderer](../gui-renderer.html) §11.

**Fields we write: `main+0x37E3F`, one byte, directly — and, INDIRECTLY, whatever the engine's own
redraw writes when we ask for one.** This line read "none" until 2026-09-21, then "ONE, and it is
new"; the landing review showed that second answer was also short, and the difference matters
enough to state first. The direct write is the memo byte. The indirect one is that
`repaint_service` calls `0x4A81E0(gi, 0x40)`, which reaches `0x4A16F0` and writes `gi+0xCCA`
(`0x4A1708`) and the gadget's `+0x1F` (`0x4A1722`, `0x4A1758`) — engine fields, written by engine
code, on a frame we asked for. **The argument that used to cover this does not.** It ran "the
engine calls this with `0x40` itself, so the path is one it already takes", and the engine's pump
at `0x4AA0C5` actually passes `[GUIMEM+0x10] | 0x40` — never a bare `0x40`. Ours is a subset of
those bits, so it cannot *add* an allocation, but the precedent was being claimed for an argument
value the engine never passes, and "no allocation, no free under `0x40`" is therefore **asserted,
not established**. What IS established is the measurement: repaints ran across every screen in the
fixture set without a fault, and the ops came back semantic. *Named as an open gap rather than
closed.* The module reads the engine's surfaces, the palette and the mouse
object — and, since G15d, the fork's own palette object under the fork's lock — and writes GL
objects of its own. On top of that it now **poisons byte 0 of the 33-byte last-drawn memo** the
METAL/ENERGY block inside `DrawGameScreen` compares itself against, once per twin reset, to make
the engine redraw a block it would otherwise skip for the life of the level ([engine
map](../exe-reverse-engineering.html) "The resource block, and the 33-byte memo that skips it").
The poison is the complement of the byte the block will write there (`PlayerStruct+0x146` of
the local player, the player index bounded to the eleven records first: record 10 is a replay's
viewer's). Every engine writer of
that field stores 0..10 (engine map), so the poison, 0xF5..0xFF, is a value the block cannot
compute and the compare cannot come out equal; a poison that alternated `0xFF`/`0x00` did on every second reset for player 0, and the
bars then stayed blank until metal or energy first changed.

What keeps the memo write inside this module's contract rather than breaking it: the memo has
**three** references in the binary — `0x468E51` and `0x468FC6` inside that block, plus `0x4679A6`,
which zeroes four dwords of it and never touches byte 0 (this read "exactly two references" until
the landing review searched for a third) — and every one of them is display code, so it is a
display memo and not sim state; byte 0 is written by the block from the player record at `0x468E7F` and never read back out
of the memo, so no value the engine displays is ever invented by us (bytes 1..4 ARE read back, as
the number's animation state, and are deliberately left alone); and the write happens on the game
thread at the flip's return, after that frame's block has already run. The engine's behaviour with
the module armed is therefore **one extra redraw of its own resource block per reset**, not a
changed value — but it is no longer byte-identical, and a reader looking for what this module
touches must find this here.
**G17a (2026-09-09) did not change that**: it added a GL texture, an FBO and shader arithmetic
and reads no engine address the module did not already read. **Nor did G17b**, which is
fork-side and tooling: the client-area → engine-logical pointer transform moved out of
`wndproc`'s button cases into `mouse_client_to_game` (`mouse.c`) so the harness's device-space
click takes the same path a player's does, the UI snapshot gained the frame's `viewport`, and the
native pass gained `devres` — at `k > 1` `ss` follows `ceil(k)` and the box-resolve to the game's
resolution is skipped, so the composite downsamples the supersampled buffer instead of
nearest-stretching a game-res one (replication 42.6 % → 11.2 % of adjacent device pixels at
`k = 1.5`, fps unchanged). **It is OPT IN — `tagpu_devres.on`** (it was on by default with
`tagpu_devres.off` as the A/B until the rect measurement above; `devres=` is on the native log
line either way); at `k = 1` it is inert.

### 2.104 A free is an op: the save-under's restore keeps its place (`tagpu_gui_hook.c`, `tagpu_gui_leaves.h`) — 2026-09-27

Escalation's orders panel stayed on screen after a deselect: the panel's teardown copies its
save-under back onto the frame at `0x4A952B` and frees it with the next call, `0x4A9537`, and the
producer dropped the surface at the free — `PK_FREE` at once, the copy's source cleared — so the
copy crossed as `PK_PIXELS` and was dropped. A game-thread free now **retires** the entry: it
stays, marked dying, an `OP_FREE` is recorded at the free's place, and `publish` emits the copy
twin to twin and then `PK_FREE`, in the engine's order. Nothing reads a retired surface's memory
(`pub_surface_bytes` refuses one), and `pub_seed` carries no bytes any more — the drain dropped
them — so a surface made, drawn and freed in one window is seeded and drawn like any other.
`GUI pixels:` counts every `PK_PIXELS` by cause; `copy-freed` fails the suite's UI check, and the
suite's battle stage compares the side panel after a deselect with the engine's surface.
Measured: Escalation 22.5 % of the strip wrong on v0.3, 0.12 % with the fix; `copy-freed` 0.
Mechanism, residuals and numbers: [GUI renderer](../gui-renderer.html) §28. **Not covered:** an
off-thread free, a same-base re-make and a full window still drop at once.
