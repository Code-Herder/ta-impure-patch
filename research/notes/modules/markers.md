# World-space UI markers

The marker pass and the selection rectangle.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.2 World-space UI markers (`markown`, `markown.on`)

| VA | What it is | Mechanism |
|---|---|---|
| `0x4699EB` | `call 0x46A530` — selection rect, ground sweep | call-site redirect, **per-unit** test |
| `0x469B8A` | `call 0x46A530` — selection rect, air sweep | call-site redirect, per-unit test |
| `0x469BD7` | `call 0x471F90(ctx,8)` — hook 8, the marker block's start | call-site redirect; since G13p it opens no capture — it brackets the order arena's block and **latches `[globals+0x204]`/`+0x208`**, the font and text colour the block's own text would use |
| `0x469D2C` | `call 0x471F90(ctx,9)` — hook 9, the block's end | call-site redirect; closes the order arena's block |
| `0x469CF9` | `call 0x4C14F0` — the group digit | call-site redirect; the engine's call is **skipped** and the digit re-drawn out of our text atlas (G13p) |
| `0x469EC5` | `call 0x4BF8C0` — build-cursor rect | call-site redirect; the engine's call is **skipped** and the rect re-drawn (G13n) |
| `0x469F1E` | `call 0x4BF8C0` — drag band box | call-site redirect, same — one rect, one gate, both re-drawn |
| `0x439516` | `call 0x439740` — target sprite, from the route-dot drawer | call-site redirect, **trace only** since G13p (the identity blend LUT went with window A) |
| `0x439C7D` | `call 0x439740` — target sprite, from the walker's bit-3 dispatch | call-site redirect, same |
| `0x46A430` | `DrawHealthBars` (`ret 0x10`) | prologue detour, 5 stolen — bars are **re-drawn**, not captured |
| `0x4C1B80` | `KeyboardHotkeySampler(id)` (`ret 4`) | *called by us* — we sample the engine's own SHIFT gate (`0xF9`) rather than reading the key |
| `0x469BFC` | `call 0x48CC30(ctx, main+0x142F3)` — the order-marker driver, SHIFT-gated | call-site redirect; our stub **snapshots the order lists on the game thread** and the engine's driver is then **skipped** (G13o). `passive`/`trace` make the snapshot decline, so the engine draws its own |
| `0x439BAC` | `call 0x438C00` — build-site rect, from the walker's bit-0 dispatch | call-site redirect, **trace only**: logs the engine's node list under `order.on=trace`, otherwise a compare and a call through |
| `0x439BF2` | `call 0x4394E0` — route dots, bit 1 | call-site redirect, trace only |
| `0x439C37` | `call 0x4399F0` — target circle, bit 2 | call-site redirect, trace only |
| `0x439CBE` | `call 0x4390A0` — range circles, bit 4 | call-site redirect, trace only |
| `0x465AC0` | `UnitInPlayerLOS(player, unit)` (`ret 8`) | *called by us*, on the game thread, to reproduce the target sprite's LOS rule and its last-seen cache write |
| `0x485070` | `GetPosHeight(POS16_16*)` (`ret 4`) | *called by us*, on the render thread — a pure read of the height grid, which is what makes a range circle follow the terrain |
| `0x4CCF60` | the glyph blitter (**cdecl**, 9 args, base and pitch taken directly) | *called by us*, on the present thread, once per distinct string — it reads the font object and writes our atlas and touches no engine state at all (`tagpu_text.c`) |

**Everything anchored to a unit takes the unit pass's sub-pixel anchor — including, since
2026-09-09, the health bar and the group digit — and every one of them is quantised AFTER the
zoom, not before.** Two separate defects, found and fixed the same day, and the second was
created by the first fix.

**(a) The anchor was the wrong one.** Until 2026-09-09 the bar gather read the engine's integer
world shorts directly, which pinned the bar to the SIM rate while the body glided at present
rate; the two slid against each other by up to a whole sim step of motion. The old error was
**proportional to how far the unit moves per sim step**, so it grew with unit speed and with
`gamespeed` — 1.68 px peak-to-peak at 1x at TA's normal speed, 2.95 at `gamespeed` 20, and
`zoom` times either on screen.

The rule now is **the bar sits on whoever drew the body**, and it takes two branches because
two different things draw units. When `tagpu_native_owns_unit()` holds — the very predicate the
unit pass gathers on — the body came from `tagpu_native_unit_pos()`, so the bar takes that same
number. When it does not, the unit pass skipped the unit and the **engine** drew it from `(s16)`
reads of the same 16.16, and the bar floors with it. That second branch is not hypothetical:
`markown` suppresses the engine's own bars globally, so a unit the type filter rejects still
needs a bar from us. (A nanoframe while the build-effect detour was absent was the other case until
2026-09-23, when the native pass began owning every nanoframe — [build-state](../build-state.html) §7.)

*[The landing review caught this. Both branches went through the accessor at first, on the
belief that it reports "no sub-pixel sample" — it does not. `tagpu_native_unit_pos` returns 1
whenever its **pointer** checks pass and hands back the raw fraction when the table holds no
sample, so the integer branch was unreachable and every engine-drawn unit got a bar up to a
whole game pixel off its body. The code comment and this note both asserted the opposite, that
the sampleless path was byte-for-byte what it had always been.]*

**(b) Then it was quantised on the wrong grid.** The first fix floored that anchor, on the
argument that the selection rect floors the same anchor and the two should agree. They did
agree — with each other, in the frame's PRE-zoom units, which is the wrong grid. The vertex
shader scales this pass by `zoom` about the zoom centre, so **one unit of quantisation here is
`zoom` displayed pixels**: invisible at 1x, 4 px at 4x, 8 px at `ZOOM_MAX`. The bar stood still
and then teleported 4 px while the body glided underneath it, which is what the owner reported
as a diagonal twitch at max zoom-in. (The selection rect never showed it because it does not
actually floor in these units — `tagpu_native.c` snaps its corners *forward through the zoom*,
floors there and comes back. The glyph atlas has done the same since G15.) `tagpu_mark.c`'s
`snap_device()` is now that rule, shared by the bar, the group digit and the text, so the step
is **1/ss of a displayed pixel at every zoom** and the bound does not grow with the zoom at all.

**Measured** on a walking commander, 1920x1080, camera pinned, TA's normal game speed, at 4x —
the separation between the bar and the body it sits over, in *displayed* pixels
(`tagpu_spxlog.on`; the model is exact and build-independent, which is what makes one run report
all three rules):

| the bar's anchoring rule | rms | peak-to-peak |
|---|---|---|
| the engine's shorts (the original defect) | 1.35 px | **6.56 px** |
| the body's anchor, floored (fix 1, superseded) | 1.15 px | **4.00 px** — exactly `zoom` |
| the body's anchor, `snap_device` (ships) | 0.14 px | **0.49 px** — 1/ss, at any zoom |

Corroborated from video on the two builds themselves, same instrument each leg
(`tools/barwobble_detect.py`, `scenarios/bar-wobble-4x.json`). The statistic that catches this
is **the bar against itself**: a unit walks at constant speed, so a bar that tracks it has a
second difference near zero and a bar quantised on a grid of `q` px stands still and then
teleports `q`. Before: still on **60.9 %** of presented frames, and every step it did take was
exactly 0, 4.00 or 8.00 px with nothing in between; |2nd diff| 2.09 px mean, **4.00 px p99**.
After: still on **2.3 %**, a continuous 1.1–3.2 px spread tracking the unit's real speed;
|2nd diff| 0.42 px mean, **1.00 px p99**. At 1x both builds pass every 1x oracle equally —
that is the point of (b), and the reason the 4x fixture is now a tracked one.

**Stock TA cannot show either artifact**, because there the bar and the body are the same shorts.

**The selection rect is drawn at 1x, after the downsample, and matches the engine's pixels.**
Four things had to be right and none of them was ([UI markers](../ui-markers.html) §1, all measured
2026-09-08 against the engine's own rect — `mark.on=noselbox` hands it back while everything
else stays ours, so both boxes land in one `glshot`):

1. **The rotation.** The four corners take `0x4B6CC0`'s triple — `Rz(u+0x64)` on `(x,y)`,
   `Rx(u+0x68)` on `(y,z)`, `Ry(u+0x66)` on `(x,z)` — which is what `0x467A50` does to these
   same points. The loop used the *transposed* yaw until 2026-09-08: a rotation by −heading, so
   a unit turning on the spot had its box turning the other way (2× the heading off — invisible
   at every multiple of 45°, obvious between them). The tilt words are not decoration either: a
   tank on a hillside carries up to 17° of bank and 31° of pitch.
2. **The bounds.** `0x4CB650(model,&min,&max,**0**)` seeds min and max with the ORIGIN, skips
   any node with fewer than three vertices, and with that last argument 0 never leaves the root
   node — so the rect is the root piece's own vertices, not the whole tree. Ours was the whole
   tree and stood ~11 px taller with the bottoms aligned. `aabb_walk` is unchanged (it is the
   shadow pass's model height); the rect has its own `selbox_aabb`.
3. **The arithmetic.** The engine truncates each term of the projection separately and halves
   the height with `sar 1` *after* truncating it. One float expression instead is a pixel out on
   some edges: 46 of ~110 box pixels differed until that was reproduced, 7 after.
4. **The line.** The engine's is Bresenham (`0x4BE950`): one fully coloured pixel per major-axis
   step. Ours was a GL line in the 2x supersampled FBO, and **the driver clamps aliased line
   width to 1** — `glLineWidth(ss*3)` was measured to draw pixel-identically to
   `glLineWidth(ss)` — so it was one SUPERSAMPLE wide and resolved to about half the engine's
   colour, (66,136,56) against a flat (83,223,79). It is now drawn into the **1x FBO right after
   the box-downsample**, with the supersampled depth blitted down (`GL_NEAREST`, the only filter
   a depth blit takes) so it still sits under its own unit. 100 % of our box pixels are then
   exactly the engine's colour, and its own box differs from ours on 5–18 pixels of ~110 — a
   Bresenham step landing on the other neighbour, or a pixel where ours is correctly occluded
   and the A/B's engine box (which composites over our whole world) is not.

**From 2026-09-11 it could be drawn as GEOMETRY instead, behind `tagpu_selgeom.on`; since
landing 11-3 of the vulkan-only plan that lever is GONE, and the rect went with it until §2.84
put it back on the Vulkan lane** — drawn by the marker pass, not here, and exact by a different
route (a fragment test for the engine's Bresenham). Everything in this entry describes the GL
path. On GL it was — each edge two triangles, a band of `w` game pixels
(`w=`, or `wdev=` in device pixels) expanded along the edge's *minor* axis,
because Bresenham's own rule is one pixel per major-axis step. That is the width
the driver would not give us, and it is the thing `devres` was waiting on: at
`k = 1.5` the rect reaches full colour (1019 device pixels at ≥ 0.9 coverage,
peak 1.000) where the GL line reaches 5 and peaks at 0.928. It is **bit-identical
at 1:1** — 0 differing pixels and an unmoved md5 at `ss = 2` with the resolve, at
`ss = 1`, at zoom 0.5 and 2.0, and on the slope fixture — and off by default.
`selgeom main` turned `selAt1x` off for the A/B; that cost 1320 pixels at
`ss = 2`, so on GL the 1x detour was what made the default exact. **That detour is not
expressible on the Vulkan lane** (`tagpu_vk_world.h`: the world resolves in one draw, so there
is no 1x buffer to defer the rect into), which is why both halves were deleted together rather
than one being ported. **Do not read the 1320 figure as the cost of the move** either: the
Vulkan rect is not that band; it is §2.93's line (DrawLine's walk on the world target, `ss`
pixels wide), and §2.84 has what it measured when it kept whole game pixels.
[UI markers](../ui-markers.html) §1 has the numbers and the two construction traps
(the cap must run along the segment; the band is nudged 1/256 px off the tie).

Two consequences worth knowing. **Supersampled — the default — the rect draws after the marker
layer**, so where a box edge crosses a health bar our line wins where the engine's bar would (the
bars sit inside the box on every stock unit measured); the fallback site under `tagpu_ss.off`, or
without `glBlitFramebuffer`, still draws it first, under them, so the two sites layer differently. And the same supersampling that dimmed the rect dims **every line
and glyph the marker layer draws** — bars, order lines, range circles, the text atlas — which is
the same fix one layer up, and is not done.

**The order block is PORTED, not captured, since G13o.** `tagpu_order.c` re-derives §3 of
[UI markers](../ui-markers.html) — the driver's three selection rules, the walker's
capability-mask dispatch and its `pos` chaining, and all five leaf drawers — and draws them
at native resolution in `tagpu_mark.c`'s pass. A capture could never reach past the
OFFSCREEN's own width and height, and the offscreen is screen-sized while `vpwide` lets the
projection run far outside it, so at zoom < 1 the engine's own clipper threw the outer ring's
markers away before our buffer saw them.

**And the text went the same way in G13p.** The bound on a string is not even a clip:
`DrawTextCustomFont 0x4C14F0` measures the whole box, tests it for CONTAINMENT in the
context's clip rect (`0x4C6AE0` + `0x4B6750`) and draws nothing at all if it does not fit. So
a group digit or a `ShowRanges` label in the outer ring at zoom < 1 vanished rather than
stopping short. `tagpu_text.c` calls the blitter underneath, `0x4CCF60`, which takes a base
and a pitch directly and has no bound of any kind, with `(fg,bg,transparent) = (255,0,0)` —
that turns it into a 1-bit coverage mask, so one raster per string serves every colour it is
drawn in. Each string lands once in a shelf-packed 512×256 atlas; the quads are constant
SCREEN size and snapped to the device pixel grid by pre-image, because a bitmap glyph at a
fractional anchor resolves each 1-px stroke across two device pixels.

**Window A is retired, and the identity blend LUT with it.** `0x439740` is the only
alpha-composited marker: it reads the destination pixel through `tab[(src<<8)|dst]`, and while
we captured the block into a buffer of ours that destination was the fill key, so the waypoint
star blended with palette 254's bright cyan and rendered teal. The two call sites were
therefore bracketed with an identity LUT — a 64 KB table and a pointer swap scoped to the call
because `[globals+0xC0]` owns a heap buffer the engine allocates, frees and refills
(`exe-reverse-engineering.md` §"The blend LUT and the marker composites"). With nothing of the
engine's landing in a buffer of ours, the star composites against the engine's own frame again,
exactly as stock does, and all of that machinery is gone. Under `passive` and `trace`, where
the engine draws its markers into its own key-filled surface and we composite that surface,
the star is teal — a debug mode's business, not the shipped frame's.

### 2.84 The selection rect, back on the screen — drawn by the marker pass, exact by a fragment test

**What was wrong.** A selected unit showed no selection rectangle. The rect lived only inside
the GL unit draw that landing 11-3 deleted (§2.65), and nothing replaced it; §2.65 recorded the
engine as drawing "every box itself", which stopped reaching the screen at the clean cut (§2.81):
the engine's box lands in the golden source and nowhere else. Reported from play.

**What draws it now** — [UI markers](../ui-markers.html) §1, *On the Vulkan lane*, has the design;
in short:

| where | what |
|---|---|
| `tagpu_native.c` `selbox_emit`, `selbox_aabb` | the GL pass's geometry, unchanged: root-piece bounds (cached per template node, dropped in `cache_gen_check`), `0x4B6CC0`'s angle triple, `0x467A50`'s truncating projection, zoom snap; emitted in the unit hand-over loop at key `enc − 0.5`, ahead of every skip |
| `tagpu_mark.c` `tagpu_mark_emit_selbox` | the bucket and the ownership gate (armed, not `passive`, not `noselbox`); the rects are the first draw of the list and its only `depth` draw; the vertex grows to 8 floats (clip z, 0 for every other marker) |
| `tagpu_mark.c` `SVS`/`SFS` | a program of its own: the line is a band extended along the segment and `ceil(3·scale) + 2` target px wide, and the fragment stage keeps a sample only when its GAME pixel is on `DrawLine`'s Bresenham (`0x4CC7AB`) |
| `tagpu_vk_mark.c` `s_pipeLineZ` | that program, Bresenham line mode, depth test `LESS`, writes off; `uPx` written at record time from the target's real extent (`FS_SZ` 48, `FS_PX` 32) |

**Why a fragment test rather than a line.** The first cut drew the rect with the pass's own
Bresenham line pipeline at `ss` width. In the `ss×` target a diagonal steps half a game pixel at
a time and the composite's box filter smears it: **28 of 290** rect pixels at full colour, the
rest at 0.2–0.8, visibly faint beside the engine's. The GL answer (draw at 1x) has no buffer on
this lane (`tagpu_vk_world.h`). Deciding membership per game pixel makes every sample of a kept
pixel survive, so it resolves to the flat colour at any `ss` and under `devres`.

**Measured 2026-09-23**, `selbox-facings` (three Stumpys at facings 45/135/200), 1024×768,
`ss = 2`, the bench arm set, selected minus deselected on both the reference surface and the
window in one run:

| measurement | number |
|---|---|
| our rect pixels identical to the engine's own box | **176** of the engine's 261 (the rest: nearer trees in our frame — the reference has none — and health bars) |
| our rect pixels the engine does not have | **5** |
| our rect pixels at exactly the engine's (83,223,79) | **173 of 181** (the line pipeline: 28 of 290); the 8 are on hull silhouettes, where the body's supersampled edge shares the pixel |
| depth test off, same fixture | the rect's front tip drawn over the next row's tree; the stock game (nothing armed) hides it, as the depth-tested build does |
| `500v500`, all own units selected | `sel=401 selover=0`, 60 fps |

**The landing review found four, all acted on.** The band was extended along the major axis
alone and drawn `2·scale + 2` wide, which a brute force of Vulkan's wide-Bresenham rule showed
leaves samples of kept pixels uncovered at every scale above 1 (58 of 3 720 edge vectors at
`ss = 2`, 1 328 at 3) — now along the segment and `ceil(3·scale) + 2`, with none uncovered from 1
to 6 in 1/16 steps. A zero-length edge (a root piece under three vertices) drew nothing where the
engine plots a pixel — now split a quarter of a screen pixel. A failure to build the rect's own
pipeline took the whole marker layer with it — now it drops the rects alone. And a comment in
`tagpu_native.c` still said `s_sbox` was deleted. The table above was re-measured on the fixed
binary.

**Not covered.** The occlusion was compared against the stock game by eye on two trees, not
swept. The rect's lines are now §2.93's: a band and an integer test on the line grid (the world
target's pixels), `ss` pixels wide, with DrawLine's clip (`0x4BEA20` to the viewport, then
`0x4CC650`) applied to their ends where the record is built, so the rect follows the engine at
the viewport edge and needs no device line feature. At `ss = 2` that line steps by target pixels
— smooth, by the owner's ruling — so on a window the game frame's size it no longer resolves to
the flat colour the table above measured ([UI markers](../ui-markers.html) §1). The line-primitive rule and the width requirement described above belong to the
pipeline §2.93 replaced.

**Found on the way.** On `500v500` the unit pass stood down for the whole frame
(`vk: unit: the GL twin drew 348 posed unit(s) this hand-over does not carry`): about 860 units
were on screen against `TAGPU_PD_MAXHAND`'s 512, so **no unit body drew at all** while the rects
did. §2.86 removes that cap.
