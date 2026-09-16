# G19f — the UI layer and the present, planned

**Written** 2026-09-16, at the start of the gate. The roadmap's G19f row is the gate; this is
how it is cut into landings and why. Nothing here is measured yet — every number in it is a
target, and this file is deleted or folded into `gpu-status.md` when the gate closes.

## What is being ported

`tagpu_gui_surf.c`'s render half ([gpu-status](gpu-status.html) §2.3e). It is not one draw over
a mesh like every world pass: it is a **stateful store of per-surface twins** that an op stream
mutates, plus a three-layer composite. The state is the hard part and it is what decides the
cut below.

Its inputs, on the render thread, today:

| input | where it comes from |
|---|---|
| the op stream | `tagpu_gui_int.h`'s lock-free SPSC queue — 65 536 ops, a 16 MB byte arena |
| the twins | one `RG8` texture per engine surface (R = palette index, G = coverage), created by `PK_SEED`, mutated by `PK_PIXELS`/`PK_CLEAR`/`PK_SPRITE`/`PK_COPY`/`PK_STRING` |
| the UI GAF atlas | `TAGPU_GAFATLAS`, 2048², already has an opt-in CPU mirror (§2.3e) |
| the palette | `tagpu_pal.c`, already ours and already a snapshot (§2.3f) |
| the sharp layer | one `RGBA8` at device resolution — cursor (G17c), strings (G17d), minimap (G17e) |
| the colour twins | Classic++: a second `RGBA8` per surface, written by MRT (G15e) |

Nine shaders are **already translated** — `inc/spirv/tagpu_gui_surf.spv.h` carries `QVS`,
`CPY_FS`, `SPR_FS`, `STR_FS`, `CURS_FS`, `MM_FS`, `SHARP_FS`, `LAY_VS`, `LAY_FS`. G19c did
this. The port writes no new GLSL.

## The one design decision the lane's own rules force

**THE OP STREAM IS DEAD BY THE TIME THE VULKAN LANE RUNS.** `render_ogl.c`'s iteration is
`tagpu_packet_acquire` → `tagpu_overlay_draw` (inside which `tagpu_gui_present` drains) →
`tagpu_packet_frame_end` → `tagpu_vk_frame`. `drain()` advances `g_guiq.aTail` **per op** and
`qTail` at the end, so the instant it returns the game thread may overwrite the arena those ops
point into. A Vulkan pass reading `g_guiq.arena + o->aoff` would be the terrain pass's fog-grid
bug again, on a bigger buffer.

Two ways out, and the lane has already chosen between them once:

* **Defer the tail publication** until after `tagpu_vk_frame`. Cheapest, and wrong here: the
  release would have to run on every path through `tagpu_overlay_draw` including its three early
  returns (the same argument `tagpu_cursown_publish`'s comment makes), and it makes the GL
  queue's lifetime depend on a lane that is normally not armed.
* **Copy what the pass needs, into an arena of our own, while the ops are live** — which is what
  `tagpu_terr.c` does with the buffer `glTexImage2D` was handed, what `tagpu_gaf.c`'s opt-in CPU
  mirror does, and what the unit pass's hand-over rows do. **This is the one taken.**

So: `tagpu_gui_surf.c` grows an **opt-in hand-over** — asked for by the Vulkan pass, filled
inside `drain()` as each op is applied, carrying a copy of each `TAGPU_PUBOP` and a copy of its
arena payload, stamped with the frame. `TAGPU_VKPASS::frame` refuses any other frame, per the
rule the terrain re-review established. **It costs nothing when the lane is not armed**, which
is the only reason a per-frame copy of the op stream is affordable at all.

The Vulkan pass then applies **the same ops through the same shaders** to twins of its own. It
is not a second implementation: the twin state evolves identically because the op stream is
identical, which is the only claim an A/B can check.

## The landings

One per landing, each with its own A/B, never as one drop — the lane's standing rule.

**Landing 1 LANDED 2026-09-16** (`main` `fdc8dbd..23d61d0`): the 1x mirror, 0 px on the shell at
640x480 and in game at 1024x768 and 1920x1080. Three review rounds, sixteen findings, all acted
on — including a use-after-free and, in the first round of fixes, a reseed storm that changed
the GL oracle. [gpu-status](gpu-status.html) §2.34 is the module note.

**Landing 1 — the 1× mirror.** The hand-over, the twin store, `PK_SEED`, `PK_PIXELS`,
`PK_CLEAR`, `PK_SPRITE`, `PK_COPY`, the UI atlas mirror, and the `LAY_VS`/`LAY_FS` composite
with the sharp layer empty and no colour twins. `PK_STRING` and Classic++ are **refused**, in
the shape every world pass refuses what it does not carry.

*Its A/B is exact and the GL side already has the levers:* `gui.on=nostring nocursor nominimap
norestore` empties the sharp layer and the colour twins, so the GL lane draws the mirror alone
and the two halves are comparable. Without those four tokens there is nothing to compare
against, which is why this is the cut rather than a smaller one — a landing that could only be
measured on a screen with no sprites could not be measured at all, and the shell redraws every
gadget on every flip.

**Landing 2 — THE STRING OP, and it is not the sharp layer.** `STR_FS`. Landing 1 shipped with
`PK_STRING` standing the whole pass down, and since text is on screen in essentially every in-game
frame, that means **the pass composites nothing in real play** — it only works under
`gui.on=nostring`. Closing that is worth more than anything else in G19f, and it is smaller than
it looks:

* **A string op writes the TWIN, not the sharp layer** (`twin_string`, G17d — the render thread
  stamps TA's own glyphs into the twin from a per-font glyph cache). So it extends landing 1's
  store rather than needing landing 2's machinery. §2.3e's "the sharp layer is empty until the
  cursor and the string op fill it" is about the SHARP text path, not this one.
* **The glyph atlas is already a CPU array.** `tagpu_text.c` keeps `s_gatlas[GA_W * GA_H]` and
  uploads the GL texture FROM it, and `tagpu_text_glyph_gen()` already exists. So the "how does a
  second backend get the texels" question — which cost the feature pass a whole mechanism — is
  answered here by one accessor.
* What the hand-over must carry, per op: `fg`/`bg`/`tr`, the destination box, and **the resolved
  per-glyph cells** (`ax`, `ay`, `w`, `h` and the pen position), because `twin_string` resolves
  them against an atlas that **can repack mid-string** (it retries once for exactly that reason) —
  so re-resolving on the other side would be re-deriving the pass's inputs, which this lane does
  not do.
* `STR_FS` is already translated (G19c) and takes `uSize`, `uFg`, `uBg`, `uTr` plus the atlas.

**Landing 3 — the sharp layer proper.** `SHARP_FS`, `CURS_FS`, `MM_FS`: the cursor at device
pixels and the sharp minimap. Drops `nocursor` and `nominimap`.

**Landing 4 — Classic++.** The colour twins and the MRT sprite/copy programs, the per-texel
choice between restored colour and the live palette, and the palette-validity rule. Drops
`norestore`. **Blocked on the restorer's five shaders** (G19c's own uncovered case) if the UI
atlas needs restoring in the Vulkan lane — check before starting; it may be that the hand-over
can carry the restored texels the GL side already produced, which would unblock it.

**Landing 5 — the present.** The clause the roadmap's row names that nothing above touches:
the frame presented through Vulkan with the fork's ddraw path intact, and the shell↔game
context switch clean. This is where route D stops being a second window.

**Landing 6 — the frame-time gate.** `frame time no worse than GL` on the 200v200 fixture at
1920×1080, sim paused, 281 units and 76 wrecks, `--maxfps 0`. **Nothing in Phase G has measured
cost at all**, so this landing is mostly a harness: a way to read whole-frame time on each lane
that is not the readout's own `fps=`, and a floor measured on one binary twice before any
comparison is believed. Budget it as its own piece of work rather than a line at the end of
landing 4.

## What this plan does not know yet

* Whether a **Vulkan twin per engine surface** is affordable. The GL side has one texture per
  surface with no stated ceiling; in a 32-bit address space with `VkImage` granularity this may
  need a pool or an atlas instead, and that is landing 1's first measurement, not an assumption.
* ~~Whether the composite reads `gl_FragCoord`.~~ **CHECKED 2026-09-16, and the gate does NOT
  stall on §2.33's decision.** Exactly one of the nine shaders reads it — `CPY_FS`, the
  twin→twin copy — and its target is a TWIN, which is SAMPLED rather than presented. §2.32's
  rule ("the flip is a property of presentation") therefore applies in our favour: the copy
  takes no negative viewport height, so the mirror that makes the unit pass stand down cannot
  arise. `LAY_FS`, `SPR_FS`, `STR_FS`, `MM_FS` and `SHARP_FS` read it nowhere.

  **DERIVED 2026-09-16 against `twin_copy` and `QVS`, and the answer is that `CPY_FS` ports
  unchanged with no flip.** The paragraph that stood here said the GL lane "already contains a
  mirror between its fragment coordinate and its texel index" and that the port had to reproduce
  it. **That was wrong**, and it was reasoning from the convention instead of from the code —
  the thing this bullet told itself not to do. The chain:

  * `QVS` maps `y / uSize.y * 2 - 1` to NDC y, so quad y = 0 lands at NDC −1, which is
    attachment row 0.
  * GL measures `gl_FragCoord.y` from NDC −1 — i.e. from attachment row 0, which for an FBO
    whose colour attachment is the twin IS texture row 0.
  * `twin_upload`'s `glTexSubImage2D(…, l, tp, …)` writes texture row `tp`, and `twin_copy`'s
    `glScissor(o->l, o->t, …)` restricts to attachment row `o->t`. Both address the same rows
    as the quad does.

  So in GL, `gl_FragCoord.y`, the texel index, the scissor row and the quad's y are **one
  number**, and `p = gl_FragCoord.xy - uOff` is a direct texel index. In Vulkan with a POSITIVE
  viewport height, NDC −1 is attachment row 0 and `OriginUpperLeft` measures `gl_FragCoord.y`
  from attachment row 0 — **the same number again**. The two conventions differ only when the
  viewport height is negative, which is exactly the presented case §2.32 already carved out.

  `vkCmdSetScissor`'s `offset.y` and `vkCmdCopyBufferToImage`'s buffer row 0 agree with it, so
  the entire twin path — seed, pixels, clear, copy, sprite — is convention-identical between the
  lanes and needs no transform anywhere. Only the composite flips, because only the composite is
  presented.

  The module being flip-free and top-down in storage is otherwise a gift: `vkCmdCopyBufferToImage`
  puts buffer row 0 at image row 0 exactly as `glTexSubImage2D` does here, so seeds and pixel ops
  need no transform at all. `LAY_VS`'s `1.0 - a.y*2.0` pairs with the negative viewport height
  every presented pass takes, which puts clip y = +1 back at attachment row 0.
* Whether the shell's ~12 000 flips/s publish cadence makes the hand-over copy dominate.
