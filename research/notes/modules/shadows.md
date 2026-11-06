# The Classic hard shadow

The Classic shadow on the Vulkan lane.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.83 The Classic hard shadow, put back on the lane that draws — and the default

**What was wrong.** No unit and no structure cast a shadow on the shipped configuration, in any
mode. Measured 2026-09-22 at the play defaults (`pose-inventory`, Two Continents, 1024x768):
`classicpp.cfg=shadows=0`, `=1` and `=2` gave a **pixel-identical viewport** — 0 differing pixels
of 896x704 between 0 and 1, 13 between those and 2, and those 13 were one animating 4x4 spot.
The engine's own frame had the shadow at the same camera, and its option word was not the
problem: `main+0x37F06` read `0x000C003F`, so bit2 Shadow, bit3 TShadow and bit4 FShadow were all
set. Feature (tree) shadows drew throughout — that is `tagpu_feat.c`'s own path and was never
involved.

**Why.** Landing 11-2 deleted `tagpu_native.c`'s GL tail and the silhouette and slant draws went
with it; 11-5d then deleted the eight `tagpu_posedraw_*` entry points they had called, correctly
noting that they had no caller left. `tagpu_vk_unit.c` drew the BODY range and said so under
*WHAT IT DOES NOT DO*. So the gap was a consumer gap, not a producer one: the bake has carried a
SLANT range since G16 step 6 and the posed vertex shader has carried `uRange`, `uOffset` and the
fragment stage's `uShadow` the whole time. **No shader changed in this landing and no SPIR-V was
regenerated.**

**What it is now.** Three files, one decision each.

| file | its half |
|---|---|
| `tagpu_native.c` | **the policy**. `pk->gfx_opt` (the frame packet's copy of `main+0x37F06`) and the Classic++ `shadows=` key decide, per unit, one of `TAGPU_PDSH_NONE` / `_SIL` / `_SLANT`, plus `shOffY = gy - ay`, the shift onto the ground line under the unit |
| `tagpu_posedraw.c` | **the range**. `pd_record` turns the kind into a `first`/`count` — the BODY range for a silhouette, the bake's SLANT range for a structure — and hands it over. A ghost casts nothing |
| `tagpu_vk_unit.c` | **the draw**. A third record stage, before the bodies, two pipelines and two more uniform windows a unit |

**The engine's option word is read from the packet, not across the threads.** `pk->gfx_opt` is
`RDU8(ta, OFF_GFXOPT)` in the publisher, on the game thread, exactly as the marker pass reads
`damagebars` (bit0) out of the same byte. Bit2 gates both kinds; bit3 gates the SILHOUETTE alone,
because the engine tests it only on the completed-unit branch (`0x459324`) and never on the
structure one. Bit4 is the feature pass's and is untouched.

**Two pipelines, because one 50 % blend per silhouette PIXEL is not one per surface.** This is the
GL twin's stencil dance restored: mark the silhouette into the stencil with colour writes off,
then blend where the mark is with the op that ZEROES it, so a second fragment on that pixel fails
`EQUAL 1`. Both draws see the same depth buffer with depth writes off, so they cover exactly the
same fragments and no mark outlives the unit that made it — and two DIFFERENT units' shadows still
stack, as the engine's two separate blits do. Without it a building's slant compounds wherever two
faces project onto the same ground.

**So the seam's depth attachment carries a stencil plane now.** `tagpu_vk_world.c` already named
the stencil aspect in its view and cleared it (its own comment: *"a view of them must name it, or
the framebuffer is refused"*); `tagpu_vk.c` did the opposite, on a comment claiming a view with
both aspects "cannot be used as a plain depth attachment on every driver". The world module is
right and the seam now matches it: aspect mask, `stencilLoadOp = CLEAR`, `depthStencil.stencil =
0` in the clear value. `TAGPU_VKPASS.stencilok` says whether the chosen depth format has the plane
at all, and a device without one gets **no hard shadow** rather than an unmasked one. Same shape
as `flipok` and `lineok`. Both formats the seam takes carry the plane (§2.91), so it is 1 wherever
there is a depth format.

**`shadows=` now defaults to HARD (2), and that is a consequence rather than a preference.** SOFT
was the default and has drawn nothing since landing 11 D2 deleted `tagpu_shadow.c`: that module
was the only producer of `TAGPU_SHADOWHAND`, so `tagpu_vk_shadow_prepare` returns 0 on every frame
and the consumers' refusals are gated on the same `shadowOn` that goes to 0 with it. Both halves
went dark together and nothing said so. **The soft map is still unported** — reviving it is
writing the producer (the light basis, the map extent, the heightfield caster mesh), which is a
feature landing of its own.

**The render-options row follows.** `Shadows` is `Off|Hard` — two stages, not three. Offering
`Soft` would be offering `Off` under another name. `shadows=1` in a hand-written cfg is still
read; it draws nothing and the row then shows `Off`, which is what the player is getting.
`Shadow quality` (`shadowres=`) sizes that map and nothing else, so it is greyed unconditionally
instead of being left looking live.

**The structure-shadow gate has a painter again.** `s_ssSuppress` had had no writer since 11-3 and
is deleted; the gate now reads `tagpu_posedraw_slant_take()`, which `tagpu_vk_unit_record` sets
after it has recorded a slant. That is the "observed, not predicted" the gate's own comment asks
for — the report comes from the code that drew, one render-loop iteration before the read, which
is the one-frame cost in both directions the gate is built for.

**Measured, 2026-09-22, `pose-inventory` at the play defaults, 1024x768, paused.**

| measurement | number |
|---|---|
| `shadows=0` vs `=2`, structures camera (`eye 3100 1200`) | **2 571 px** of 630 784 (was 0) |
| `shadows=0` vs `=1` | 39 px — the soft map still draws nothing |
| `shadows=0` vs `=2`, ground camera (`eye 1716 806`, 24 units on screen) | **4 261 px** |
| the darkening, both cameras | median **0.509** of the bare terrain, single-moded (1 619 of 2 571 samples at 0.51) — the mask works; the GL twin's unmasked version was bimodal at 0.25 and 0.50 |
| against the ENGINE's own frame, one ARM solar collector on grass | our shadow's bounding box is the engine's within **1 px** on all four sides, its centroid within **1.4 px**, IoU 0.72 under one darkness classifier applied to both |
| against the engine, the eight largest mobile-unit shadows | centroid within **1 px** in x and y on every one |
| GPU frame time (`ftime.on`, `--maxfps 0`, paused, interleaved 2/0/2/0) | p50 **0.155–0.157 ms** with, **0.150 ms** without — about **6 us** |
| the whole change with `shadows=0`, against the parent build | **0 / 11 px**, which is the fixture's own cross-run floor (old vs old 0, new vs new 11) |

**The engine's Options → Visuals `Shadows` checkbox clears three bits, not one.** Measured on a
live game: `main+0x37F06` goes `0x000C003F` → `0x000C0023`, so bit2 Shadow, bit3 TShadow **and**
bit4 FShadow move together. With it off, `shadows=0` and `shadows=2` differ by **11 px** — we draw
none either, which is the whole of the requirement that the player's own toggle wins.

**Not covered.** An aircraft's shadow is drawn — visibly on the ground below a hovering airframe,
which is `shOffY` working — but it was **not** compared against the engine: the air cluster's
units are airborne on one launch and landed on the next, so the two frames are not the same
situation and the pair proves nothing. The GL twin's own rule is restored unchanged; nothing about
it was re-measured here.
