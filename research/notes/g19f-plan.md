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

**Landing 2 — the sharp layer.** `SHARP_FS`, `CURS_FS`, `STR_FS`, `MM_FS`: the cursor at device
pixels, the string op's glyph cache, the sharp minimap. Drops three of landing 1's four tokens.

**Landing 3 — Classic++.** The colour twins and the MRT sprite/copy programs, the per-texel
choice between restored colour and the live palette, and the palette-validity rule. Drops
`norestore`. **Blocked on the restorer's five shaders** (G19c's own uncovered case) if the UI
atlas needs restoring in the Vulkan lane — check before starting; it may be that the hand-over
can carry the restored texels the GL side already produced, which would unblock it.

**Landing 4 — the present.** The clause the roadmap's row names that nothing above touches:
the frame presented through Vulkan with the fork's ddraw path intact, and the shell↔game
context switch clean. This is where route D stops being a second window.

**Landing 5 — the frame-time gate.** `frame time no worse than GL` on the 200v200 fixture at
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

  **But `CPY_FS` still needs deriving rather than assuming, and it is landing 1's first task.**
  The GL lane renders into the twin's FBO, where `gl_FragCoord.y` counts from the BOTTOM, while
  `twin_upload` stores surface row `tp` at texture row `tp` — top-down — and `texelFetch`
  indexes that storage directly. So the GL lane's copy already contains a mirror between its
  fragment coordinate and its texel index, and the port must **reproduce** it, not "fix" it. In
  Vulkan with no flip, `gl_FragCoord.y` counts from the top, so the same GLSL computes a
  different `p` unless `uOff` or the quad compensates. Work it out against the code and the
  `uOff` call site; do not reason from conventions.

  The module being flip-free and top-down in storage is otherwise a gift: `vkCmdCopyBufferToImage`
  puts buffer row 0 at image row 0 exactly as `glTexSubImage2D` does here, so seeds and pixel ops
  need no transform at all. `LAY_VS`'s `1.0 - a.y*2.0` pairs with the negative viewport height
  every presented pass takes, which puts clip y = +1 back at attachment row 0.
* Whether the shell's ~12 000 flips/s publish cadence makes the hand-over copy dominate.
