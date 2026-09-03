# GPU Renderer Roadmap — Gates & Prototypes

The plan for rendering Total Annihilation's units — and eventually the whole scene — with a real
GPU 3D renderer, while the 1997 simulation runs untouched underneath. Decisions locked 2026-08-31
after a full design interview; every gate is a prototype with an exit demo and a kill/pivot rule.

## Summary

We fork **cnc-ddraw** (MIT, mingw-buildable) as our `ddraw.dll`, add an overlay-injection callback
into its OpenGL present loop, and load a companion DLL that hooks the engine, reads unit state
through the community's verified struct map, suppresses the engine's software unit blits, and draws
real 3D in their place — original 3DO geometry first, enhanced materials next, replacement glTF
models later, and a full GPU scene (terrain + features + units) as the endgame. Proton-first on
this machine; the Steam exe is stock 3.1 layout, so the entire researched address corpus applies.

> **Looking for the current state rather than the history?**
> [GPU status, hooks & limits](gpu-status.html) is the one-page view: what is ours today, every
> engine address the stack patches and by what mechanism, the known limits still to fix, and the
> rules the work converged on. It is written from the source, so it cannot drift. This page stays
> the chronological log and the gate definitions.

## Progress log

High-level gate status. Screenshots for review land under `research/site/assets/shots/` and are
linked here as they're captured. Detail is "what the gate proved", not how we got there.

| Gate | Status | Result |
|---|---|---|
| G0 — Build & inert chain | ● done | Our forked `ddraw.dll` (`7.1.0.1 git~279a057`, mingw i686) loads native from the game dir (`WINEDLLOVERRIDES=ddraw=n,b`), attaches to TotalA.exe (crc32 `520700A8`, section `[TotalA]`), brings up **GL 4.6 on the RTX 4070 under wine**, and TA renders normally through it (Cavedog splash + intro cinematic captured). Stock renderer one env-var away. |
| G1 — Overlay proof | ● done | Our GL 3.3-core overlay draws over the live game in the fork's render context — translucent (game shows through), correctly placed in the letterboxed viewport, file-toggleable, no crash at 31 fps. Built into the fork (not a separate DLL). GL-framebuffer capture (`glReadPixels`) added as the review path for overlay content. |
| G2 — State-read proof | ● done | Live skirmish, vs AI: we resolve the game struct, walk the unit array, project each unit through TA's own world→screen formula and draw a GPU marker on it. Confirmed visually — a green marker (own-unit colour) sits exactly on the ARM Commander. Same loop scales to every unit. Menu driven under the locked session by keyboard accelerators (`space`→Single, `s`→Skirmish, `Return`→Start) + mouse-edge scroll. |
| G3 — Static frame map | ● done | One frame's composition fully mapped from the binary ([Frame composition](frame-composition.html)). Key insight: the Lock/Unlock heartbeat is the *present* (`FlipOffscreenToPrimary 0x4C63A0`, called last) — all drawing happens earlier into a software offscreen buffer. Chain: `GameFrame_InGame 0x496790` → `DrawGameScreen 0x468CF0` in painter order (terrain `0x418310` → interleaved feature/unit row sweep → fog `0x420B00` → UI → present). Per-unit path isolated: **`DrawUnit 0x45AC20`(offscreen, unit)** → COB pose-bake `0x45B0A0` → composite blit `0x459200` with per-pixel 8-bit z-merge `0x4B90A0`. Sprite-cache rasteriser found (`0x4586A0`): TA re-rasterises the whole 3DO on every move/animate — not a static per-orientation bake. Hook table frozen, 24 functions decompiled through the new Ghidra project. **Per-unit blits are isolatable** — no pivot needed; G5 stays a single detour at `DrawUnit`. |
| G4 — Runtime confirmation | ● done | In-process tracer (5-byte detours, byte-match guarded) proved it live in a skirmish: **`DrawUnit 0x45AC20` draws the on-screen unit** (all calls via site `0x469A00`; the second call site never fires → no shadow pass), and **blit `0x459200` composites it 1:1** carrying the unit's Object3do + a projected screen position — confirming the `DrawUnit → 0x458810 → 0x459200` chain. New finding: DrawUnit is **LOS/frustum-gated** — `DU=BL≈3250/window, distinct=1` with the commander in view, `0` when it's off-screen or the camera is on the fogged enemy base (our overlay still marks those, reading sim memory). Open-question #1 resolved: `Object3do+0x10` is a **persistent per-unit composite** (stable when idle), so it's the G6 write target. Hook list frozen. |
| G5 — Suppression | ◐ done (observational) | Detour on **`DrawUnit 0x45AC20`** (stdcall/`ret 8`; suppressed path is `pushad→classify→popad→ret 8`, writes nothing) hides one **unit type** by name. Live: ARM Commander suppressed (>277k draws skipped), its **sprite gone** while the map/foliage/features render normally and our overlay still marks it — selective, render-only. Probe confirms the suppressed unit is a **complete live model with intact `pos`/`turn` sim state**. Formal replay-byte-diff needs an unlocked session (clicks don't land under lock), same as G2. |
| G6 — Sprite-cache stepping stone | ● done — real GPU geometry blitted by TA | Full circle, live (2026-08-31). **Read:** `unit+0x9E → Object3do → posed PrimitiveStruct[]` chain binary-confirmed ([Unit → 3DO bridge](unit-3do-bridge.html)). **Write:** composite format + palette RE'd, write-back channel proven with a gradient ([Composite buffer](composite-buffer.html)). **Render:** the gradient is now replaced by a **real GPU render of the engine-posed 3DO** — ~185 triangles from the posed vertex buffers, TA's dimetric projection, per-face colours sampled from the resolved GAF textures, index-exact 8bpp readback into the colour plane — and TA blits *our* commander, visually matching its own sprite ([render3do](gpu-render3do.html), shots below). Remaining Phase B polish: real texture UVs, per-player team-colour frame, own depth plane, all unit types, then own-the-draw (suppress + own repose). |
| G7 — One unit, truly GPU | ● done (composite-level) | Delivered via **own-the-draw** rather than an RGB overlay: the engine keeps pose-sync/AABB/alloc/hotspot/blit, our GL FBO supplies every pixel (index-exact 8bpp + own MRT depth plane). The commander walks, animates and team-colours correctly in the 4-AI skirmish ([own-the-draw](own-the-draw.html)). The "palette lifted to RGB" half of the original exit moves to the native-res gate (G12) — the 8bpp composite is the colour-fidelity ceiling. |
| G8 — Compositing correctness | ● done (composite scope) | By staying inside TA's compositor we inherit occlusion, fog, z-merge, selection UI and water for free — the 4-AI test shows CORE+ARM bases, engine particles and fog coexisting with our pixels. Phase C RE closed the theory gaps: **build-state** is a blit-time effect on the composite driven by `unit+0x104` and the depth plane as height map ([build-state](build-state.html)); **shadows DO exist** — blackened-silhouette alpha blits inside `0x459200`, option-gated ([shadows & cloak](shadows-cloak.html)); **cloak** = 50% blend via the ALP table, enemies never draw it; site `0x469BA3` = the airborne sweep. Live verification of each on our pixels is the remaining work. |
| G9 — All units at speed | ◐ | `all`/`all` renders every visible unit exclusively on GPU (many types at once, +10 sim speed, no crash). Formal 500-unit stress skirmish + 60 fps + MP-safety audit still pending. |
| G10 — Materials & light | ● done (8bpp scope) | **Per-face directional shading shipped** (2026-08-31): per-triangle normals from the posed verts (winding-independent — flipped into the view hemisphere), sun from screen upper-left, brightness = palette-index remap through a 256×32 nearest-match LUT built from the live palette (row 16 = exact identity; reserved indices never emitted). Verified in the 4-AI skirmish on ARM vehicle plant / CORE solar / commander, `tagpu_shade.off` gives in-run A/B ([render3do → shading](gpu-render3do.html), gallery below). Next: build-state visuals, glow-face exemption, AA experiments. |

**Legend:** ● done · ◐ partial · ○ pending · ✕ blocked

### Where the GPU port stands (2026-09-02)

| What the engine used to draw | State | Owned how | Verified how |
|---|---|---|---|
| Units (every complete unit) | ● native RGB, `tagpu_native.c` | `owndraw` detours skip the software rasterisers | same-fight A/B, 200v200 at 60 fps |
| Wrecks (3DO husks) | ● native | scratch-unit draw suppressed by the owndraw classifier | A/B on `one-wreck` / `shadow-mix` |
| Unit shadows, cloak, waterline | ● native, engine rules incl. FBI gates | part of the unit pass | A/B `shadow-mix`, `waterline` (Anteer Strait) |
| Weapon fire, explosions, debris | ● native (G12e) | `fxown`: two call-site redirects + four leaf detours | A/B `fx-lasers`/`fx-mix`/`fx-rockets`, engine surface empty of effects |
| Smoke, fire, wakes, nanolathe | ● native (G12f) | one detour on the layer walker `0x471F90` | A/B `sfx-strait`, engine surface empty of particles |
| Features (trees, rocks, splats, wreckage) | ● native (G13a) | `featown`: one detour on the leaf `0x46A610` | occlusion parity vs the engine's own draw, engine surface empty of features, `feat-forest` |
| Fog of war *as drawn* | ✅ **at parity** (G13c, 2026-09-02) | one shared rule (`tagpu_glsl.h`) in all four native passes, off the engine's own screen fog grid | done — [Features](features.html) §9 |
| Terrain tiles | ● native (G13b) | `terrown`: one detour on `0x483FA0`, whose skip path key-fills the viewport | 0-px parity vs the engine's own blit, engine surface 99.9 % key, in-process map change |
| Fog overlay | ● native (G13b) | `terrown` detours `0x4848E0` too, replicating only its lazy grid rebuild | 99.06–99.39 % lit-vs-grey agreement with the engine's own overlay |
| Health bars, order markers, group digits, build cursor, band box | ● native (G13d, corrected G13h) | `markown`: eight call-site redirects + one detour on `0x46A430`; bars re-drawn, the rest captured out of the engine's own draw and replayed. G13h fixed the capture's publication discipline and gave the waypoint star an identity blend LUT | engine surface 99.98 % key with only the cursor left, bar geometry exact (33×3 fill at the engine's x), markers scale with the world at 0.5×; **0 overlay dropouts in 840 held-SHIFT frames** (was 13 in 120) and **0 % cyan** on the star (was 17.6 %) |
| Chat, dialogs, side panel, minimap, top bar | ○ engine 8bpp, through the composite key | — | stays engine-side: screen-space, correct at 1:1 at any zoom |
| Mouse cursor | ● moved in the composite (G13e) | the cursor is the ONLY engine pixel left inside the viewport, so the composite paints the box around `u` at the box around `s` | full sprite at the pointer at 0.25×/0.5×/1×/2×, in every corner and in the display-only ring |
| Click → world point | ● transformed (G13e) | `tagpu_zoom.c`: one rewrite at the three doors into the engine's own wndproc, plus `fake_GetCursorPos` | at 0.5× the commander selects at its DRAWN position (394,427) and no longer at its 1× one (212,470); the side panel still clicks 1:1 |
| Minimap view rectangle, scroll rate | ● zoom-aware (G13e) | `0x466B70` ×2 redirected and its rect rescaled by 1/z; `ScrollSpeed` (`main+0x1434D`) driven at base/z | minimap box doubles at 0.5× and quadruples at 0.25×; ScrollSpeed 32 → 64 → 128 → 16 at 2×, and restores |
| The camera's range at zoom > 1 | ● follows the zoom (G13g) | `tagpu_zoom.c`: the eye clamp `0x41C3C0` replaced by a `leaf_call` detour while zoom > 1, widening `[0, map − W]` by `d = (W/2)(1 − 1/z)`, plus a map clamp on the `GetTPosition` inside `0x498DA0` | measured on Two Continents at 1024×768: (−224,−176) at 2×, (−392,−308) at 8×, (10048,12144) at the far corner, all exact; 1× and 0.5× land on the engine's own (0,0)/(9824,11968) |
| The addressable ring at zoom < 1 | ● closed for input (G13f, **opt-in** `vpwide.on`) | `tagpu_vpwide.c`: the engine's own viewport rect widened to the transform's range, with the clip (`0x4C6B10` ×3) and the screen→world origin (`0x498DA0`) redirected and corrected, plus a signed `lParam` unpack in TA's wndproc | at 0.5× a ring click selects the unit under it in all four quadrants and a right-click walks it to the world point clicked; 1× untouched; the captured markers reach the offscreen bound, not the frame edge |
| Atlas cell edges at zoom-out | ● closed (G13i) | every atlas cell carries a 1-texel replicated border (terrain on a 34-texel pitch; GAF frames advance `w+2`/`h+2`), plus `TAGPU_EDGE_NUDGE` — 1/32 game-screen px in the terrain VS, after the zoom scale | the period-8 row anomaly at zoom 0.25 goes 1.92×/2.05× → 1.03–1.13× at every camera phase, `ss=1` and `ss=2`, 1024×768 and 1920×1080; ten zoom levels clean; **1× output bit-identical** to a build without it |

So the engine's software frame is now **UI only** — inside the viewport it is a flat fill of
one palette index, the *key*, with nothing on it but the mouse cursor (G13d took the rest). Everything a player looks at
in the world is ours. That inverts the composite: instead of dropping our empty pixels so the
engine's frame shows through, we drop our pixels wherever the engine's frame is **not** the
key — which is the entry condition for the declared endgame, ortho + smooth zoom.

**Phase 0 is complete** (G0–G2 all green): our forked `ddraw.dll` drives TA on a real GPU, draws our own OpenGL over the live frame, and reads live engine state to mark units in a running game. The foundation for the GPU unit renderer is in place.

**Phase B is structurally complete** (2026-08-31, one session): G6 done with **real textured GPU geometry** (atlas-sampled GAF textures, index-exact 8bpp path, own MRT depth plane, `all`-types mode — [render3do](gpu-render3do.html)), and **own-the-draw proven**: two byte-guarded detours skip TA's software rasterisers (`0x459830`/`0x459C70`) for chosen types while the engine keeps pose/AABB/alloc/hotspot/blit — a forced rebuild leaves the engine plane 100% ColorKey while the on-screen unit is entirely our render ([own-the-draw](own-the-draw.html)). Remaining Phase B verifications, gated on a visible non-P0/moving unit (fog + locked-session clicks): the player→team-frame mapping and a walking-pose render; both landed in the registry-preset 4-AI skirmish (Painted Desert, LoS=Permanent): the white AI's ARMCOM shows the white team badge — `frame[owner]` mapping correct — and the commander was captured mid-stride. **Phase B verified complete.**

**Phase C is underway** (2026-08-31): three static-RE agents run in parallel — build-state/nanoframe internals (`0x459C70`, the rasteriser `mode` arg, the build-progress field), shadows/cloak (`0x4B8500` shade tables, the never-firing second DrawUnit site `0x469BA3`), and terrain/feature depth (`0x418310`, the row sweep, the z-merge destination) to unblock the native-res design (G12). All three RE notes are back ([build-state](build-state.html), [shadows & cloak](shadows-cloak.html), [terrain & depth](terrain-depth.html) — the latter corrects the frame map: real terrain `0x483FA0`, real fog `0x4848E0`, and **no screen depth plane exists**), and the native-res architecture is drafted ([native-res design](native-res-design.html)). Main session shipped G10 shading + 2x supersampled edges same day.

### Awaiting review

**G13i — the interior cracks, and a note that said they were impossible.** Reported from
play: *"at max zoom out there are a lot of crack artifacts — black line on the right side of all
the map features like trees, and blue-ish or sometimes black lines on all sides of map tiles,
most often top/bottom."* Both are one bug. Quads are emitted on integer game-pixel boundaries, so
at zoom 0.25 a quad's far edge lands exactly on a fragment centre; the rasteriser hands that
fragment to the upper/left quad, its `u`/`v` interpolates to exactly `u1`/`v1`, and `GL_NEAREST`
resolves that to the first texel of the **next atlas cell** — tile index +64 for terrain (an
unrelated tile, blue over forest because Two Continents' tile set is mostly water) and the shelf
packer's never-written gutter for a sprite (index 0 = black, and not the frame's colour key, so
it survived the key test). It fires on the **parity of the camera**: one world pixel of eye
movement turns it on or off, which is exactly the "not always present" in the report.

Two halves, two fixes. A **1-texel replicated border on all four sides** of every atlas cell
stops the sample leaving the cell — and is what a filtered sampler will need when these atlases
stop being `GL_NEAREST`, which is why it is four-sided and not two. But padding alone is not
enough under `GL_NEAREST`: the fragment then repeats the cell's last row, which is out of phase
with the cadence the rest of the 4×-minified tile is sampled on, and TA's tile art is dithered,
so it is still a line — measured, 1.92× → 1.86×, i.e. no help at all. So the geometry moves too,
by 1/32 of a screen pixel, applied after the zoom scale so it is the same sub-pixel distance
everywhere; a coincident fragment centre then falls inside the *following* quad and samples the
texel it is standing on.

**The expensive part was a note.** [terrain & depth](terrain-depth.html) §7.1 asserted that atlas
bleed was impossible because "interpolated `u` stays inside `[u0, u1)` for any fragment centre
inside the quad" — and a fragment centre can land exactly **on** the far edge, which is the one
value that interval excludes. That sentence ruled tile seams out of the hunt, and 350+ frames
were swept for a key leak that was never there; the detector, `min(r,b) − g`, was measuring the
key's tint and this defect never touches the key. §7.1 now carries the correction and §7.6 the
mechanism. A seam that is periodic in screen space wants a periodic detector: score each row
against its own two neighbours and group by `y mod (32·z)`.

**G13h — the order overlay stopped flickering, and the waypoint star stopped being teal.**
Both reported from play, both in `markown`, and both older than the gate that shipped them.

*The flicker.* `mark_hook8` cleared the published marker layer on the way into every capture
and only republished it at hook 9. That hole is open for the length of one capture — and the
engine runs that block far more often than we present, because with the stack armed everything
else in its frame is skipped and ours is the slow half. **Measured on a live skirmish at
1024×768: 9 300–10 200 hook-8 blocks per 120 presented frames, ~80 per frame the player sees.**
The GL thread reads the publication on cnc-ddraw's render thread, which leaves the primary
surface's critical section long before `tagpu_overlay_draw`, so it landed in that hole **13
times in 120 presents** — roughly every eighth frame had no order markers. Deciding "nothing
this frame" *before* the capture and leaving the last publication standing otherwise took it to
**0 in 840**. The second buffer already existed for exactly this; what was missing was the rule
that a publication is only ever *replaced*, never emptied and refilled. Two buffers turn out to
be enough, and that was measured rather than assumed (0 in ~50 000 publications for the case
where the writer reclaims the slot the reader still holds) — it rests on the upload finishing
inside two of the engine's blocks, so it is the number to re-take if the layer ever grows much
faster than the block does.

*The star.* The pulsing sprite at a waypoint is the one alpha-composited marker: `0x439740`
goes through `AlphaCompsteBuf2OFFScreen 0x4B8500`, which reads the destination pixel and looks
the pair up in `tab[(src<<8)|dst]`. Stock TA's destination is the terrain, so the star reads
olive over grass; since G13b ours has been the fill key, so it blended with palette 254's
bright cyan and looked washed out. **17.6 % of the sprite's box was on the cyan ramp; it is
0 % now.** The fix is an identity LUT — every pair answering `src` — which turns that one
composite into a copy, and the replay then draws it **opaque**: a deliberate departure from
stock's blend, chosen over re-blending against our own scene, which would now be a shader
change rather than another capture change.

Three things this gate is worth remembering for:

- **`markown.h` had already predicted the star bug and dismissed it** — "what they see
  underneath is the fill key, which is exactly what they already read out of the engine's frame
  today". True of what the primitive READS, wrong about what it WRITES. Both that comment and
  `ui-markers.md` are corrected in place rather than deleted; the wrong inference is the useful
  part.
- **The blend LUT pointer is not a hook point.** `[globals+0xC0]` owns a 64 KB heap buffer:
  `0x4BA5C0` allocates it, `0x4BA5F0` frees it from the graphics teardown, `0x4BAAD0` refills
  64 KB *through* it. The first revision installed the swap at hook 8 and restored it at hook 9,
  reasoning that a global can safely be put back a frame late — and the review caught that. It
  cannot: `0x469C03 je 0x469D38` skips hook 9 whenever `drawUnits == 0` (TA's own movie
  recorder, `0x495E88`, is the one caller that passes 0), and the star is drawn *before* that
  branch, so an abandoned frame would leave our pointer to be clobbered or cross-heap-freed.
  The swap is now bracketed around the drawer's two call sites — a call that always returns.
- **What the reviewer had to correct in the prose, twice.** That `0x439740` is "not in a
  function-pointer table" (it is, 19 times in `.rdata`; the field is simply never read in this
  build), and that `DrawTranspRectangle 0x4BF8C0` reads a destination (it does not — it is
  named for its hollow centre, and the band box rendering as a clean white outline instead of
  washing out like the star is the visible proof). Addresses in
  `exe-reverse-engineering.md` §"The blend LUT and the marker composites".

**G13g — the camera's range follows the zoom.** Reported from play: *"when you zoom in and try
to get to the edge of the map it's impossible — the engine pushes your camera back as if you
were still at 1:1 zoom level."* Exactly right. `0x41C3C0` clamps the eye to `[0, map − W]`,
which puts the **1× viewport's** edges on the map's; at zoom `z` the view is still centred on
`eye + W/2` but is only `W/z` wide, so the visible window stopped `W/2 − W/(2z)` short of every
map edge — 224 px at 2×, 392 px at 8× on 1024×768. The range is now the engine's own widened by
exactly that `d`, so the visible window's edges land on the map's at both extremes. Everything
downstream came for free, because the eye *is* the engine's camera: minimap box, "centre on
unit", the minimap click jump, the HotUnits cull, our passes.

Three things made it small rather than a camera rewrite:

- **The widening is the transform's own arithmetic**, about the same centre, so the two cannot
  disagree at the edges — the world at the viewport's left edge is `eye + d`, zero exactly at
  `eye = −d`. Measured to the pixel: (−224,−176) at 2×, (−392,−308) at 8×, (10048,12144) at the
  far corner, (−239,−188) at the wheel's 1.1⁸ = 2.144.
- **The flag keeps 1× byte-identical.** A `leaf_call` detour raised only while a zoomed-*in*
  world is live; clear, the engine's own function runs verbatim *including the two minimap-rect
  redirects inside it*. 1× and 0.5× land on the engine's own (0,0)/(9824,11968).
- **An off-map eye needed a guard, and only one.** For a pointer *outside* the viewport
  `0x498DA0` answers `eye` itself (side panel) or `eye + H − 1` (bottom bar) — the
  `GetGridPosPLOT`→NULL→`GetGridPosFeature` crash `vpwide` already carries a clamp for — so its
  `GetTPosition` call is redirected and the world point clamped. A pointer *inside* needs
  nothing: at `z > 1` the transform maps the viewport into `[L+d, R−d]`, so the world it names
  is `[0, map−1]` at either extreme. Verified by sweeping the panel and both bars at 2× and 8×
  with the eye negative: alive, cell (0,14), no feature. Our terrain pass was already general
  about negative tiles (`tile0=(−10,−7) off-map=346 junk=0`), and the viewport black fraction at
  the corner is 0.000515.

`apply_eye_range()` re-applies the same bounds once a frame, because the engine clamps only when
*it* moves the camera — without it a zoom-out at a map edge left the eye parked off-map until the
next scroll — **and it must clamp the scroll target `main+0x14327`/`+0x1432B` with it.** The
review caught that one: the correction has no caller to copy the eye into the target afterwards,
and the stepper `0x41CA30` acts on any disagreement — `0x41CB5F` sets the camera-moved bit and
`0x41CB6B` clears `main+0x14281` bit 3, the fog grid's own is-current flag, then halves the
distance and hands it to the no-longer-widened engine clamp, which puts it straight back. That is
a permanent per-frame fog-grid rebuild after any zoom-out from a map edge, on exactly the path
`97e518f` had to guard against a crash. The replacement clamp deliberately does *not* write the
target: three of its callers are inside that stepper, and doing so would stop the camera arriving.

`tagpu_zoomedge.off` is the live off switch and puts the eye back on the 1× range.
`tacli eye`'s own clamp was the same bug in the scripted path and now shares the range.

**Known gap, and it is the scroll target that draws the line.** Three sites compute that target
and clamp it *inline* against `[0, map − W]` without ever calling `0x41C3C0` — `0x41C4C0` (smooth
`SetCamera`), `0x41C7F7` (smooth centre-on) and `0x41CAF7` (per-frame camera **follow**). The
stepper walks the eye to that target and our wider clamp leaves it there, so **those paths still
stop `d` short of a map edge**: track a unit into a corner at 4× and the camera stops where 1×
would. Nothing fights and nothing churns — the eye arrives at a target inside our range and both
stop. Closing it means widening three inline clamps in the middle of the camera module, which is
a bigger patch than this one.

**G13f — the ring at zoom < 1 is a play mode (opt-in, `vpwide.on`).** G13e's honest answer
to the ring was to *drop* the click; this addresses the ring instead. `tagpu_vpwide.c` widens
the engine's own viewport rect — `main+0x37E27..0x37E33`, never W/H — to exactly the range the
zoom transform produces, so the routing test, `GetUnitAtMouse` and the **HotUnits cull** all
follow the view. Verified at 0.5× on `feat-forest`: a ring click selects the unit under it in
all four quadrants (`ARMCOM1.GUI` vs `ARMMAIN2.GUI`) and a right-click into the ring walks it
to the world point clicked.

Three things had to be true for that to work, and two of them were not obvious:

- **`0x498DA0` uses L and T as the screen→world ORIGIN**, not as a bound
  (`world = eye + clamp(pos, L, R) − L`). Measured: moving L 128→0 and T 32→0 shifts the map
  cell under the cursor by exactly (+8, +2) cells. Its one call site is redirected and the
  conversion redone with the true origin and the wide clamp.
- **TA's own window procedure zero-extends the mouse `lParam`** — `AND 0xffff` / `SHR 0x10` at
  all three arms of its `0x200..0x206` jump table, the `LOWORD`/`HIWORD` idiom. A client x of
  −20 arrived as 65516 and the event was lost, so hover worked and clicks did not, for exactly
  `x < 0` or `y < 0` — half the ring. The byte patch is `GET_X_LPARAM`, and for any position a
  real mouse can report it is bit-for-bit identical.
- **The offscreen's clip rect comes from the same field** through `0x4C6B10`, which is a bare
  four-dword store with no clamping, so those three call sites are redirected and clamped to the
  surface. Otherwise a wide rect would license any engine drawer still running inside the
  viewport to write outside its allocation.

A third, found by the code review and reproduced in the frame: **the engine can NAME more than
it can DRAW ON.** Widening the addressable rect also moved where the engine put its cursor, and
in the ring that is the side panel or off the surface — at 0.5× with the pointer at screen
(320,400) there was no cursor at the pointer and a ghost one on the build panel. The cursor poll
now has its own transform that keeps G13e's ring identity while the messages carry the widened
`u`. The same ambiguity — engine coordinates `[0,128)` reached both by a ring pointer and by a
pointer on the panel — is why the `0x498DA0` stub settles "world or UI?" from the true pointer
position rather than from the coordinate it is handed.

And the lesson the survey did not predict: **the engine is not defensive about inputs its own
eye clamp made impossible.** An edge scroll at 0.5× took an access violation at `0x421E64`
reading `[NULL+8]` — the widened clamp reaches world points the 1× viewport never could,
`GetGridPosPLOT` returns NULL outside the plot grid, and `GetGridPosFeature` dereferences it.
Every widened value now has to be brought back into range before it is handed to engine code.

The marker half is **improved, not closed**: the capture reaches the engine's screen-sized
offscreen instead of the 1× viewport, and beyond that the engine cannot draw at a negative
position into a screen-sized buffer ([UI markers](ui-markers.html) §6.1).

**G13e — zoom is finished: the click, the cursor, the minimap and the scroll rate.**
G13d made everything the player *looks* at ours and scaling as one thing; what was left
was that everything the player *does* was still 1:1, so at any zoom ≠ 1 every click
landed on the wrong world point and the engine's cursor sat visibly away from the
pointer. All four halves are now closed, and the whole of it hangs off one small module,
`tagpu_zoom.c`, which owns the transform `u = (s - c)/z + c` and is the only place that
knows it.

- **The click.** The transform is applied at the three doors into the engine's own
  window procedure (`wndproc.c` ×2, the shield's `to_game`) and at `fake_GetCursorPos`
  — never at the many places that *write* `g_ddraw.cursor`, because cnc-ddraw's
  PeekMessage rewriter and its wndproc both normalise the same event and a transform at
  a write site would be applied twice. `g_ddraw.cursor` keeps the TRUE pointer position;
  only what leaves for the engine is unzoomed. Outside the world viewport it is the
  identity, so the side panel, minimap and top bar stay 1:1 — verified by clicking their
  gadgets at 0.5× and 2×.
- **The cursor** — see [terrain & depth](terrain-depth.html) §7.7. Moved in the
  composite, not captured: the cursor is the one thing in the frame the G13d capture
  trick cannot reach, because it is blitted with a NULL context.
- **The display-only ring, and the honest answer to it.** The engine can only name
  screen positions inside its own viewport (measured: a click outside does nothing at
  all). At zoom < 1 the view shows more world than the 1× viewport has room to name, so
  the outer ring is *display-only* — the same boundary the captured marker layers stop
  at. There the transform returns the pointer unchanged, which keeps hover, edge scroll
  and the cursor working exactly as at 1×, and the **button event is dropped whole** —
  the virtual key state as well as the message, because the engine polls that too.
  A click in the ring leaves the selection alone instead of selecting whatever happened
  to be at the 1× position. **G13f closes this for input** (below); the drop is what
  still happens with `vpwide.on` absent, and it stays the safe fallback.
- **The minimap view rectangle and the scroll rate**, both zoom-aware, both by letting
  the engine do the work and adjusting the result.

**G13d — the world-space UI markers, and zoom-out fills the frame.** The last engine pixels
inside the viewport anchored to a *world* position, and therefore the last thing between us
and a free view zoom: a marker the engine draws is a marker frozen at the unzoomed
projection — invisible at 1×, a ghost at anything else. `tagpu_markown.c` + `tagpu_mark.c`
take them with **two different mechanisms**, because they split cleanly in two
([UI markers](ui-markers.html) §6, and the new "third rule" in
[own the draw](own-the-draw.html)).

**Health bars are re-drawn**, and `DrawHealthBars 0x46A430` is detoured away. They cannot be
captured: the engine's loop walks **HotUnits, culled to the unzoomed viewport**, so a
captured bar layer would stop at the 1× rect and leave the outer ring of a zoomed-out view
bare. Ours walks the unit array with the zoom's effective rect. §2.1's arithmetic is
reproduced exactly — the *unsigned* `(Health<<5)/maxHP`, the `maxHP/3` thirds, `DrawBar`'s
inclusive edges — and verified in the frame: a full-health fill is **33 × 3 px at exactly
the engine's x** (bar at `x-0x10` for a unit whose roster screen x is 266). It also exposed
a live bug it had been hiding: `tagpu_native.c`'s native selection rect emitted palette
index **10** where the engine emits `gui[0xA]` = **233**, drawing the box in a dark colour
that nobody had seen because the engine was still painting its own green one over it.

**Everything else is captured and replayed.** The order-marker pass is five drawers over the
order list with a growing build rect, a marching dot phase, an LOS cache and `ShowRanges`
text labels; re-deriving it is a lot of arithmetic to get subtly wrong. Instead the engine
draws it into a scratch 8bpp buffer of ours — the `OFFSCREEN` is a **stack local**, so its
pixel base is one pointer to redirect — and we upload that buffer and draw it as a quad
through the same zoom transform the world uses. Parity is exact by construction, text
included. **Two windows because fog divides them**: hook 8 `0x469BD7` → hook 9 `0x469D2C`
(order markers + group digits, fog-darkened like the engine's) and the two
`DrawTranspRectangle 0x4BF8C0` calls (build cursor + band box, never darkened). Six
call-site redirects, no collision with `fxown`/`terrown` because the stubs *call*
`0x471F90` and `0x4BF8C0`. The selection rect's own two sites are redirected through a
**per-unit** `tagpu_native_owns_unit` test, so a unit the native pass does not own keeps
the engine's.

**Verified:** engine surface **99.98 % key with only the mouse cursor left** (112 px of
630 784) — chat, dialogs, panel and minimap are screen-space and stay; `prefog=captured`
with SHIFT held and the route dots, target sprite and selection box all in our frame;
`postfog=captured` on a drag band box; bar fills exactly halve (33 px → 16 px) at 0.5×
with every marker still over its unit.

**And the zoom gathers are finished.** `tagpu_feat.c` was the last pass still sizing itself
from the engine's viewport; it now uses the effective rect like the others, and the terrain
budget went from a **bail** (which hands the draw back and flashes — the one behaviour that
looks like a bug) to a **clamp**: `tagpu_terr_clamp_span()` trims the rect to what `MAXCELL`
can draw before any pass reads it, so every gather agrees on one centred rect and extreme
zoom-out degrades to an honest black margin instead of a flash. Measured black fraction
inside the viewport: **0.0019 at 1×, 0.0003 at 0.5×, 0.0002 at 0.35×** — the zoom-out
margin the G13b probe filmed is gone.

**Gaps, both deliberate.** The *captured* layers are clipped to the 1× viewport (the
engine's drawers clip to the OFFSCREEN rect), so at zoom < 1 order markers and group digits
stop at the unzoomed edge while the world carries on — health bars, the always-on markers,
do not have this limit. And **input under zoom is still 1:1**: making clicks land needs
either owning the engine's cursor draw or accepting a cursor that visibly detaches from the
pointer, which is its own gate.

**G13b — terrain, native, and the composite inverts.** The last layer the engine painted in
the world. The drawing half is small — `0x483FA0` is a grid blit of pre-rendered 32×32 tiles
and nothing else, so ours is one R8 atlas built once per map (2048×2560 for Two Continents'
5062 tiles) and one quad per visible cell at a depth key under every other band — and it lands
at **exact parity**: zero differing pixels against the engine's own blit in every terrain-only
band, at arbitrary sub-tile scroll.

The gate was the compositing model. Terrain covers the whole viewport, so "draw our FBO over
the engine's frame and discard where we are empty" would hide everything the engine still
paints inside it. In place of the terrain blit our detour **fills the viewport with one
palette index**, so every other index there is by construction an engine overlay, and the
composite discards *our* fragment at those pixels instead — health bars, wireframes, chat,
the build cursor and dialogs come through untouched, and the engine's offscreen still gets
cleared (that repaint is why it never needed clearing). The fog overlay is suppressed with the
terrain, since its shade remap would rewrite the key and we have reproduced it since G13c —
with its **lazy grid rebuild replicated**, because our own fog rule reads that grid. Detail,
the key choice, and the full verification table: [terrain & depth](terrain-depth.html) §7.

An inverted composite has failure modes an overlay does not, and they are worth knowing
before the next full-coverage layer: our terrain is opaque, so **emitting it without owning
the draw hides every engine overlay while still looking right** (the pass now refuses to
emit when its patch was not armed at launch, and says so); **disarming is the mirror of
arming** and needs the GL pass to keep drawing for exactly one more frame, because the
engine's already-drawn frame is still the key fill; and a screen the game thread draws
without ever reaching `0x483FA0` needs a stall timeout or the key test would black it out.
The key test is scoped to the viewport rect, and inside it a pixel neither side painted is
drawn black — so a bail degrades to black, never to raw key colour. Six ownership flips,
zero key pixels on screen. **INCOMPLETE, corrected 2026-09-03:** that rule caught only
*entirely* empty pixels; a **part-covered** one was still blended over the engine's frame
and so carried `(1 - c.a)` of the key, which drew a cyan hairline along the map boundary
at 29 of 151 zoom levels. "Zero key pixels" could not see it — a blend never equals the
key. Inside the fill our fragment is now composited over black outright
([terrain & depth](terrain-depth.html) §7.6). The gate also fixed a latent G13c bug it made fatal: `fogMode`
bit 0 was `LosType & 1`, the *mapping* option, so true-LOS-without-mapping (`LosType = 14`)
skipped the fog rule entirely — invisible while the engine drew its own overlay, a missing
grey band once we suppress it.

Gaps, honestly: health bars, nanoframe wireframes, the build cursor, chat and dialogs are
still the engine's — they come through the key, which is also exactly the machinery needed
to take them. Suppressing `0x4848E0` means the engine no longer shade-remaps its *own*
overlays under the grey band, visible only if one were ever drawn on out-of-LOS ground
(it is not). And the fog edge stays a clean threshold where the engine dithers 14 GAF
sprites — the same deliberate approximation as G13c.

<figure style="margin:0"><img src="assets/shots/terr-parity.png" alt="G13b: terrain parity"><figcaption>Parity. LEFT the engine's own terrain on its 8bpp surface, MIDDLE ours drawn over it in the same frame, RIGHT the difference — only our native units. Every terrain-only band differs by zero pixels, at sub-tile scroll offsets up to (25,31).</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/terr-ownership.png" alt="G13b: ownership and the key fill"><figcaption>Ownership and the inverted composite. LEFT the engine's 8bpp surface while we own the draw — 99.9 % one palette index, the key. RIGHT our GL frame of the same run.</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/terr-fog-ab.png" alt="G13b: fog, engine overlay vs ours"><figcaption>Fog with the engine's <code>0x4848E0</code> suppressed and ours in its place: 99 % of the viewport agrees on lit-vs-grey, and the disagreement is the 2–4 px band where the engine dithers its edge sprites and we threshold cleanly.</figcaption></figure>

**G13a — features, native.** Trees, rocks, metal patches, splats and wreckage leave the
engine's frame, and they take the depth buffer with them: a tall feature now writes depth at
the row key the engine's painter's sweep implies, so a unit standing behind a tree is clipped
by the tree instead of by the G12a scaffold's stamped silhouette. One detour on one leaf
(`0x46A610`) owns all of it. Full RE and verification: [Features](features.html).

<figure style="margin:0"><img src="assets/shots/feat-occlusion-parity.png" alt="G13a: occlusion parity, engine vs ours"><figcaption>The gate, checked against the engine's own draw rather than against a scaffold: LEFT the engine drawing its own units <em>and</em> its own features, RIGHT ours drawing both with <code>scaffold.on</code> disarmed. The five units parked two tile rows behind a tree row are clipped to the same slivers by the same canopies, and the two units one row in front stay visible.</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/feat-ownership.png" alt="G13a: ownership proof"><figcaption>Ownership: the engine's 8bpp surface with its own features (left), the same surface while we own the leaf — not one tree, rock or shrub left (middle) — and our GL frame of that run (right).</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/feat-fog-gap.png" alt="G13a: the fog gap"><figcaption>G13a's honest gap, <strong>since closed by G13c</strong> — kept because it is what put the corner-mask grid on the trail. The engine (left) draws nothing in unexplored black; we (right) drew trees across it. Both source maps read "visible" at cells the engine paints black; the overlay turned out to be driven by a corner-mask grid instead, which the native passes now sample ([Features](features.html) §9).</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/feat-scaffoldless-ab.png" alt="G13a: before and after, same fight"><figcaption>The same fight seconds apart at 2×: <code>feat.on=passive</code> (engine's features, our units floating over every canopy) and <code>feat.on</code> (ours, writing depth).</figcaption></figure>

**G12f — the particle sfx, native.** Smoke, fire, wake foam and the nanolathe spray leave the
engine: the ten "plugin-layer hook" sites of `DrawGameScreen` turned out to be the particle
layers, drawn by one walker at ten depths, and one detour owns them all. Same fight, engine
(top row) vs ours (bottom row), the 8bpp surface in the right column. Detail in
[Effects](effects.html) §7.

<figure style="margin:0"><img src="assets/shots/sfx-wake-ab.png" alt="G12f: wake foam, engine vs ours"><figcaption>A Skeeter's wake on Anteer Strait: the engine's 2×2 foam dots (top) and ours (bottom, the boat has sailed on); the engine surface loses the dots the moment we own layer 2</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/sfx-nano-ab.png" alt="G12f: nanolathe spray, engine vs ours"><figcaption>The commander finishing a nanoframe: the same green nano spray from the nano piece to the site (layer 6, over ground units); the engine surface shows only the frame while we draw</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/sfx-smoke-ab.png" alt="G12f: damage smoke, engine vs ours"><figcaption>Damage smoke over three badly damaged structures (layer 9, over everything): the same sequence frames at the same anchors, 50 % alpha in both</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/sfx-fire-ab.png" alt="G12f: burning debris, engine vs ours"><figcaption>Two extractors self-destructed, one under the engine's draw and one under ours: the burning debris (the pink-white flare sprites among the fireballs, layer 9) and the explosion pass's fireballs — the engine surface under ours carries neither</figcaption></figure>

**G12e — the effects pass, native.** Weapon fire, explosions and debris leave the engine:
same fight, engine-drawn (left) vs ours (right); the engine's 8bpp surface shows no effects
while ours draw. Detail in [Effects](effects.html).

<figure style="margin:0"><img src="assets/shots/fx-lasers-ab.png" alt="G12e: lasers and explosions, engine vs ours"><figcaption>Laser duel on Two Continents (2026-09-02): ENGINE draws (left, `fx.on=log passive`) vs OURS (right, engine skipped) — LLT lasers as lines, EMG bolts, explosion sprites, the LHT light flash (additive over a premultiplied FBO), flying debris</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/fx-laser-zoom.png" alt="G12e: laser corridor at 4x"><figcaption>The laser corridor at 4×: the same three palette colours in both (147,23,0 / 255,71,0 / 167,27,0), one game pixel wide</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/fx-ownership.png" alt="G12e: ownership proof"><figcaption>Ownership: OURS GL (top-left) with the engine surface of the same run empty of effects (top-right); flipping `fx.on` off restores the engine's draw within 30 frames (bottom row)</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/fx-explosions.png" alt="G12e: explosions and debris"><figcaption>Our sprites over the fight: EMG bolts and two-colour lasers (top), a Peewee's death — explosion sequences, flashes and 3DO debris pieces (bottom)</figcaption></figure>

**G12c — the whole base, native.** Token `all`: every complete unit on screen leaves the
8bpp path. The engine frame keeps only terrain, features and UI (note the dark footprint
clearings and the untouched metal patches); every unit pixel in the GL frame is ours —
RGB, team colours, shadows, scaffold-occluded by trees, clipped to the viewport.

<figure style="margin:0"><img src="assets/shots/g12c-all-native-base.png" alt="G12c: all units native at a CORE base"><figcaption>Marker-verified pair at a CORE base — ENGINE (left, no units) vs GL (right, all native)</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/g12c-windgens-forest.png" alt="G12c: windgens and floating maker native"><figcaption>Wind generators + floating metal maker native in the forest; a still-building windgen correctly remains engine-side (left panel, east edge)</figcaption></figure>

**G12b — the first truly native unit.** The commander leaves the 8bpp composite entirely:
RGB at game resolution, depth-tested per fragment against the G12a scaffold, own shadow.
Left pair: composite vs native, live A/B toggle. Right pair: the white AI's commander
WALKING BEHIND A TREE — its lower silhouette clipped by the canopy (scaffold discard),
while the engine frame shows no commander at all (its composite is wiped; every visible
pixel of that unit is ours).

<figure style="margin:0"><img src="assets/shots/g12b-ab-commander.png" alt="G12b A/B: composite vs native commander"><figcaption>A/B toggle: 8bpp composite path (left) vs the native RGB pass (right) — same engine-authentic colours + our shadow</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/g12c-ss-ab.png" alt="G12c: 2x supersampling A/B"><figcaption>2× supersampled native FBO (left) vs raw 1× (right) — box-filtered silhouette edges, game-res look preserved (NEAREST composite)</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/g12b-selrect-ab.png" alt="native selection rect vs engine"><figcaption>Native selection rect (left, ours + engine health bar) vs the engine's own rect (right) — same-second A/B at the same eye; the rect rotates with body yaw like the engine's</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/g12d-hires-ab.png" alt="G12d: 3DO vs replacement mesh"><figcaption>G12d replacement slot: the engine's 3DO solar through our native pass (left) vs a hot-reloaded smooth-dome OBJ (right) — same anchor, same shade/palette pipeline, geometry the 1997 rasteriser cannot draw</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/g12c-native-1024.png" alt="native commander at 1024x768"><figcaption>The native pass at 1024×768 (resolution live test): full colour, 2×SS edges, fog — zero hardcoded dims anywhere</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/g12c-shadow-ownership.png" alt="G12c: shadow ownership A/B"><figcaption>Shadow ownership (2026-09-02), rows = commander / solar / hovercraft / wreck, columns = stock engine-only, ours before, ours after, ours engine-8bpp: the engine's cached slant shadow survives the composite wipe for structures and wrecks, so ours is drawn only for mobile units; the hovercraft no longer gets a shadow the engine never draws</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/g12c-waterline-ab.png" alt="G12c: waterline clipping A/B"><figcaption>Waterline (2026-09-02), Anteer Strait, commanders 13 and 20 elevation units under sea level: stock engine, ours before, ours after, ours engine-8bpp — submerged shins/feet tinted navy and the shadow cut at the water line, as the engine does; erase for unseen enemies and digger clipping follow the same threshold</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/g12d-zoom2x.png" alt="partial zoom demo"><figcaption>Smooth-zoom prototype at 2×: native units scale about the view centre; terrain/UI stay 1× (full zoom = G13)</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/g12b-native-occlusion.png" alt="G12b native commander occluded by tree"><figcaption>The native commander behind a tree: fragments discarded against the scaffold, canopy outline exact</figcaption></figure>

**G12a EXIT — occlusion prediction vs the engine.** One second apart (the second vehicle
plant legitimately finishes its build between the frames): the engine draws the tree over the
Solar Collector's lower-left corner; the scaffold tints exactly that region and the log
predicted `occl=10% (294/2704 px)` for that building. Depth keys, silhouettes and the
painter's-order tie-breaks all confirmed against live engine behaviour. **G12a done.**

<figure style="margin:0"><img src="assets/shots/g12a-sbs-occlusion-proof.png" alt="G12a occlusion proof: engine vs scaffold prediction"><figcaption>ENGINE (left) vs GL+scaffold (right): the tree over the solar's corner is the predicted occluder</figcaption></figure>

**G12a — the depth scaffold, live on Two Continents.** Same view, seconds apart: the engine
frame vs the GL frame with the scaffold's debug tint. Every tall feature (tree, def Height=40)
is stamped from its GAF silhouette at its painter's-row depth — violet = far rows, red = near
rows; flat rocks, shadows and terrain stay untinted (far plane). This buffer is what native
unit fragments will depth-test against in G12b.

<figure style="margin:0"><img src="assets/shots/g12a-sbs-scaffold-trees.png" alt="G12a scaffold: engine vs GL overlay"><figcaption>Two Continents forest — ENGINE (left) vs GL + scaffold tint (right); the scaffold predicted the white AI commander 26% tree-occluded moments earlier</figcaption></figure>

**Phase C flagship — a full build, marker-verified.** One ARM solar collector from first
scaffold to completion, engine and ours interleaved seconds apart on the same build (phase
markers logged against screenshot mtimes — no labeling guesswork). Stage progression matches
column for column; mid-build colour differences are the scaffold's ~2Hz palette-ramp shimmer
sampled at different wave phases, and the dark under-slab in both rows is the engine's cached
nanoframe shadow. **Build-state verified end-to-end — the stage table is exact.**

<figure style="margin:0"><img src="assets/shots/phasec-sbs-solar-filmstrip.png" alt="solar build filmstrip: engine vs ours"><figcaption>ARM solar build, 8 moments across ~112s — ENGINE (top) vs OURS (bottom)</figcaption></figure>

**Phase C — engine vs ours, side by side.** Same unit, same frame, seconds apart (owndraw
disarmed for the reference shot — the engine re-rasterises nanoframes every frame, so it
wins the composite back instantly). More panels land here as the filmstrip capture runs.

<figure style="margin:0"><img src="assets/shots/phasec-sbs-vp-cargo.png" alt="vehicle plant with cargo build: engine vs ours"><figcaption>Marker-verified pair: completed CORE vehicle plant with a vehicle building INSIDE the bay — the cargo z-merge path carries our scaffold (right, crisp wireframe) exactly where the engine shows its speckle nanoframe (left)</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/phasec-sbs-mex-complete.png" alt="completed extractor: engine vs ours"><figcaption>Completed CORMEX — engine's flat fills (left) vs our per-face SHD shading + supersampled edges (right)</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/phasec-sbs-nano-reveal.png" alt="nanoframe reveal stage: engine vs ours"><figcaption>Under construction, reveal stage — engine (left) vs our in-shader scaffold (right)</figcaption></figure>
<figure style="margin:0"><img src="assets/shots/phasec-sbs-nano-early.png" alt="nanoframe early stage: engine vs ours"><figcaption>Early stage — the "engine solid mass" turned out to be the cached nanoframe SHADOW (drawn at every stage), present in both renderers; the filmstrip below settled it</figcaption></figure>

**Phase C — build-state scaffold in GL (engine formulas, our pixels).** The engine's
blit-time nanoframe effect does not fire on our written pixels, so the renderer now stages
it itself from `unit+0x104` ([build-state](build-state.html)): height-threshold reveal,
palette-ramp scaffold colours, per-face wireframe (depth-tested hidden lines).

<div style="display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start">
<figure style="margin:0;flex:1;min-width:180px"><img src="assets/shots/phasec-nano-scaffold.png" alt="mid-build: texture + wireframe scaffold"><figcaption>Ours mid-build: revealed texture + wireframe scaffold (ramp is green on this palette), engine shadow + nano spray coexist</figcaption></figure>
<figure style="margin:0;flex:1;min-width:180px"><img src="assets/shots/phasec-nano-engine-solid.png" alt="engine reference: solid ramp mass"><figcaption>Engine reference, early stage: solid ramp silhouette (our A/B stages draw wireframe only — known tuning gap)</figcaption></figure>
<figure style="margin:0;flex:1;min-width:180px"><img src="assets/shots/phasec-nano-complete.png" alt="finished windmill, shaded"><figcaption>Clean handoff to the finished model — now shaded via TA's own SHD table (engine-authentic colours)</figcaption></figure>
</div>

**Phase C — per-face directional shading (G10 first delivery) — for review.** Same unit, same
frame, shading toggled live: left-facing walls catch the sun, right-facing walls fall into
shade; palette-exact throughout (the LUT can only emit palette colours).

<div style="display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start">
<figure style="margin:0;flex:1;min-width:200px"><img src="assets/shots/phasec-shade-vp-on.png" alt="ARM vehicle plant, shading ON"><figcaption>ARM vehicle plant, shading ON — left sloped walls highlighted, inner right walls darkened</figcaption></figure>
<figure style="margin:0;flex:1;min-width:200px"><img src="assets/shots/phasec-shade-vp-off.png" alt="ARM vehicle plant, shading OFF"><figcaption>Shading OFF — flat, the Phase B look</figcaption></figure>
</div>
<div style="display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start">
<figure style="margin:0;flex:1;min-width:200px"><img src="assets/shots/phasec-shade-solar-on.png" alt="CORE solar, shading ON"><figcaption>CORE solar ON — the flat-grey wedges split into distinct lit/shaded faces</figcaption></figure>
<figure style="margin:0;flex:1;min-width:200px"><img src="assets/shots/phasec-shade-solar-off.png" alt="CORE solar, shading OFF"><figcaption>OFF — wedges identical grey</figcaption></figure>
<figure style="margin:0;flex:1;min-width:200px"><img src="assets/shots/phasec-shade-armcom-on.png" alt="ARM commander, shading ON"><figcaption>Commander ON — bright torso top, darkened flanks</figcaption></figure>
</div>

**Phase B — real GPU 3DO geometry through TA's compositor — for sign-off.** The G6 gradient
is history: the ARM Commander on screen below is **our OpenGL render** of its engine-posed 3DO
(posed vertex buffers → triangulated faces → FBO with TA's dimetric projection → per-face colours
sampled from the resolved GAF textures → index-exact 8bpp readback), written into the per-unit
composite and blitted by TA itself. Centre: ours in-game. Right: engine's own sprite (G4 shot,
with old overlay markers) vs ours — blue shoulder pads, grey torso, gold legs, gun arms all line
up. Details & the new `Model3DOFace` texture-field semantics: [render3do](gpu-render3do.html).

<div style="display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start">
<figure style="margin:0;flex:1;min-width:260px"><img src="assets/shots/phaseb-render3do-zoom.png" alt="our GPU-rendered ARM commander, blitted in-game by TA"><figcaption>Our GPU render of the posed 3DO, blitted by TA's compositor</figcaption></figure>
<figure style="margin:0;flex:2;min-width:320px"><img src="assets/shots/phaseb-render3do-compare.png" alt="engine sprite (left) vs our GPU render (right)"><figcaption>Engine's own sprite (left, with G4-era markers) vs our render (right)</figcaption></figure>
</div>

**Many-unit test (4-player AI skirmish, Painted Desert, permanent LOS, `all`/`all` armed —
engine rasterisers fully detoured, every visible sprite below is our GPU render):**

<div style="display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start">
<figure style="margin:0;flex:2;min-width:320px"><img src="assets/shots/phaseb-core-base.png" alt="CORE AI base: metal extractors, energy structures, all GPU-rendered"><figcaption>CORE AI base — extractors, energy structures, red team trim: 6 units/frame, all ours</figcaption></figure>
<figure style="margin:0;flex:2;min-width:320px"><img src="assets/shots/phaseb-arm-extractors.png" alt="ARM AI base: X-shaped metal extractors and storage, GPU-rendered"><figcaption>ARM AI base — X-shaped metal extractors + storage</figcaption></figure>
<figure style="margin:0;flex:1;min-width:200px"><img src="assets/shots/phaseb-arm-storage-zoom.png" alt="zoom: ARM storage building with teal glow lights"><figcaption>Zoom — ARM storage, glow faces lit</figcaption></figure>
<figure style="margin:0;flex:1;min-width:200px"><img src="assets/shots/phaseb-white-armcom.png" alt="the white AI's ARM commander mid-build: white team badge on the faces that are blue on ours"><figcaption>Team colour proven: the white AI's commander mid-build — <code>frame[owner]</code> picks the right badge (white here, blue on ours, red trim on CORE), green nano-spray = engine particles co-existing</figcaption></figure>
</div>

**G6 — write-back into TA's compositor — signed-off mechanism.** We reverse-engineered the per-unit
composite buffer (`GAFFrame` at `Object3do+0x10`) and TA's live palette, then wrote our own content
into that buffer in the correct 8bpp format — and **TA's own compositor blitted it onto the frame**.
Below, the ARM Commander's sprite is replaced by our animated diagonal gradient (raw palette indices,
mapped through TA's palette), hotspot-anchored at the unit's screen position, with our overlay markers
on top. This closes the integration question for the whole renderer: arbitrary pixels can be injected
into TA's software compositor. Next step is swapping the gradient for a real GPU-rendered 3DO.
Details: [Composite buffer → Write-back proven live](composite-buffer.html).

<div style="display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start">
<figure style="margin:0;flex:1;min-width:260px"><img src="assets/shots/g6-writeback-zoom.png" alt="diagonal gradient rectangle where the ARM commander sprite was, blitted by TA"><figcaption>Our content in TA's composite buffer — blitted by TA's own compositor</figcaption></figure>
<figure style="margin:0;flex:2;min-width:320px"><img src="assets/shots/g6-writeback-scene.png" alt="full map with a gradient rectangle at the commander's position"><figcaption>Whole scene — the commander replaced by our injected pixels</figcaption></figure>
</div>

**G5 — selective render suppression — for sign-off.** A detour on `DrawUnit 0x45AC20` hides one
**unit type** by name (`armcom`) while leaving everything else untouched. Left: the ARM Commander
at the exact spot [G4](frame-composition.html) showed its full sprite — now **gone**, only our green
marker over bare ground, while terrain, trees and metal patches render normally (selective,
render-only). Right: the whole scene — a complete map with the commander's sprite absent at centre.
The suppressed unit stays a complete live model in memory (`pos`/`turn` intact, COB pieces present),
so the simulation is untouched; the formal replay-byte-diff awaits an unlocked session (clicks can't
land under the locked desktop, as at G2). Details: [Frame composition → Suppression](frame-composition.html).

<div style="display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start">
<figure style="margin:0;flex:1;min-width:260px"><img src="assets/shots/g5-suppress-commander-zoom.png" alt="bare terrain with only a green marker where the ARM commander sprite was"><figcaption>Commander suppressed — sprite gone, our marker remains (cf. G4)</figcaption></figure>
<figure style="margin:0;flex:2;min-width:320px"><img src="assets/shots/g5-suppress-scene.png" alt="full map rendering normally with the commander sprite absent at centre"><figcaption>Whole scene renders — only the armcom is invisible</figcaption></figure>
</div>

**G4 — runtime hook confirmation — for sign-off.** An in-process tracer detoured the two
candidate draw sites live. With the ARM Commander in view (below, game-rendered sprite under our
green + yellow markers), `DrawUnit 0x45AC20` and the composite blit `0x459200` fire **in lockstep,
~3250 times per 60-frame window, for exactly the one on-screen unit** — proving "function X draws
unit N". Point the camera at the fogged CORE base instead (second shot: only our red memory-read
markers, black un-rendered terrain) and both hooks drop to **zero** — DrawUnit is line-of-sight
gated. Details + frozen hook list in [Frame composition → Runtime confirmation](frame-composition.html).

<div style="display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start">
<figure style="margin:0;flex:1;min-width:280px"><img src="assets/shots/g4-drawunit-commander-zoom.png" alt="game-rendered ARM commander sprite with green and yellow overlay markers, DrawUnit firing"><figcaption>Commander in view — DrawUnit/blit fire 3250×/window (distinct=1)</figcaption></figure>
<figure style="margin:0;flex:1;min-width:280px"><img src="assets/shots/g4-fog-los.png" alt="black fogged terrain with only red overlay markers, no rendered sprites"><figcaption>Fogged enemy units — overlay marks them (red), engine draws nothing, DU=BL=0</figcaption></figure>
</div>

**Phase A — unit→3DO bridge live proof — for sign-off.** Every yellow micro-marker below is one
piece of the ARM Commander's 3DO model — head, arms, torso, thighs, legs — read from the engine's
posed `PrimitiveStruct` origins in live memory and projected with the engine's own per-vertex rule
(`sx=+x, sy=−z−y/2`) on top of the unit's sprite. This is the full
`unit+0x9E → Object3do → posed piece tree` chain working end-to-end in a running skirmish; the same
probe logged all 15 piece names, transforms and posed vertex buffers
([Unit → 3DO bridge](unit-3do-bridge.html) has the log + offset tables).

<div style="display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start">
<figure style="margin:0;flex:2;min-width:320px"><img src="assets/shots/phasea-3do-pieces.png" alt="skirmish frame with yellow piece markers over the commander"><figcaption>Live skirmish — yellow piece-origin markers + green unit marker on the ARM Commander</figcaption></figure>
<figure style="margin:0;flex:1;min-width:200px"><img src="assets/shots/phasea-3do-pieces-zoom.png" alt="zoom: yellow markers on head, arms, torso and legs of the commander sprite"><figcaption>Zoom: one marker per visible 3DO piece, engine-projected</figcaption></figure>
</div>

**G3 — frame-composition map — for sign-off.** Static-analysis gate; the deliverable is the
[Frame composition (G3)](frame-composition.html) wiki note itself (annotated call graph, hook-candidate
table, pivot assessment), cross-checked in Ghidra with the imported corpus symbols.

**G2 — unit markers on live units — for sign-off.** In a running skirmish vs the AI, we resolve
TA's game struct, walk the live unit array, and draw a GPU marker on each unit using the engine's own
world→screen projection. Below, the green marker (own-unit colour) is drawn exactly on the ARM
Commander — read from memory, projected, and composited over the real game frame. The same per-frame
loop covers every unit; scrolling the view moves the markers with the world.

<div style="display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start">
<figure style="margin:0;flex:2;min-width:320px"><img src="assets/shots/g2-unit-marker.png" alt="green GPU marker on the ARM commander in a live skirmish"><figcaption>Green marker on the live ARM Commander (Canal Crossing, vs AI)</figcaption></figure>
<figure style="margin:0;flex:1;min-width:200px"><img src="assets/shots/g2-unit-marker-zoom.png" alt="zoom on the marker sitting on the commander"><figcaption>Zoom: marker glued to the unit</figcaption></figure>
</div>

**G1 — overlay proof — for sign-off.** Our own OpenGL 3.3-core geometry, drawn over the live game
inside the fork's render thread: a translucent cyan triangle (the Cavedog logo shows *through* it —
alpha blending works) and an opaque magenta triangle, both placed within the letterboxed game
viewport. Toggling `tagpu_overlay.off` removes them with no other change. Stable, 31 fps, no crash.
Captured via a new `glReadPixels` path (the surface screenshot can't see GL-drawn content).

<div style="display:flex;gap:12px;flex-wrap:wrap">
<figure style="margin:0;flex:1;min-width:280px"><img src="assets/shots/g1-overlay-on.png" alt="overlay on: cyan + magenta triangles over the Cavedog splash"><figcaption>Overlay ON — our GL triangles over the live frame</figcaption></figure>
<figure style="margin:0;flex:1;min-width:280px"><img src="assets/shots/g1-overlay-off.png" alt="overlay off: clean game frame"><figcaption>Overlay OFF — same frame, toggle file set</figcaption></figure>
</div>

**G0 — inert chain — for sign-off.** TA boots and renders through our locally-built forked
`ddraw.dll` on the real GPU (GL 4.6 / RTX 4070) under wine, with the stock path one env-var away.
Captured headless via our surface-screenshot trigger while the desktop was locked.

![G0: Cavedog splash rendering through our forked ddraw.dll](assets/shots/g0-cavedog-splash.png)

_The "FPS: 30 | Time: 0.16 ms" overlay is cnc-ddraw's own DEBUG-build frame counter, confirming the
image came from the game surface, not an X grab. Intro cinematic frame also captured
(`assets/shots/g0-intro-cinematic.png`)._

## Decisions locked

| Question | Decision |
|---|---|
| Presentation layer | Fork cnc-ddraw; add overlay callback before `SwapBuffers` in `render_ogl.c` |
| Runtime | Proton-first (Steam appid 298030); system wine 9.0 for fast iteration; Windows sanity-check late |
| Ecosystem position | Standalone stack — no dependence on TADR binaries; vendor their knowledge (MIT, attributed) |
| GPU API | OpenGL 3.3+ core, shared context, render in cnc-ddraw's render thread |
| Hook depth | Blit-level replacement, staged through a sprite-cache stepping stone |
| Endgame | **Full scene takeover** — GPU terrain, features, units; ortho camera + smooth zoom |
| Camera until then | Pixel-exact match of the engine's fixed ortho view |
| RE tooling | Ghidra (corpus imported as labels) + x32dbg under wine + `WINEDEBUG` tracing |
| Guardrails | Hooks are read-only over sim state; MP-safety checked at each gate; stock renderer always one toggle away |
| Phase 1 bar | Full skirmish, **all** units GPU-rendered, 60 fps, compositing good enough to leave on |
| Phase 2 content | Enhance original assets first; glTF exemplar pipeline second |

## Ground truth about the target

- The Steam exe (1,178,624 bytes) is **stock 3.1 layout** with one same-length import rename
  (`WINMM.dll` → `WIN32.dll`, feeding Steam's `audiere.dll` music shim). `DDRAW.dll` descriptor,
  section table, `teleporter` tag and `InitInternalCommand` all verified at corpus addresses. [VERIFIED]
- Pristine `TotalA.exe` + md5 manifest of all 80 install files preserved in `pristine/`. We never
  modify files in the Steam dir; we only *add* DLLs, and Steam Verify is the second safety net.
- TA has **no Direct3D**. Units are 3D models software-rasterised into cached 8-bit sprites (one per
  orientation) with a per-unit 8-bit z-buffer, composited into a palettised DirectDraw surface.
  "Intercepting unit draw calls" therefore means hooking engine internals, not an API boundary.
- The state we need is already mapped in TADR's `tamem.h` (Ghidra-verified): `Model3DONode` (the
  in-memory 3DO tree — fixed-point vertices, faces, GAF texture pointers, piece names) and
  `PrimitiveStruct` (the per-unit runtime piece tree COB scripts animate: `XPos/YPos/ZPos`,
  `XTurn/ZTurn/YTurn`, visibility, hierarchy). **Posing a GPU model = walking this tree.** [VERIFIED]
- Unit enumeration is a solved copy-paste: TADR's megamap iterates
  `TAmainStruct_Ptr->BeginUnitsArray_p .. EndOfUnitsArray_p` every frame and draws overlays —
  the exact read-only pattern we need, plus its hook sites bracket the engine's frame composition.
- Stock TA has **no zoom** — world→screen is scroll offset + fixed ortho. That makes pixel-exact
  matching in Phases A–B much easier than it sounds.

## What each researched project contributes

| Source | What we take |
|---|---|
| cnc-ddraw | The entire presentation layer — palette shader, windowing, edge cases; our fork point |
| TADR megamap | Unit-array iteration, world→screen math, overlay drawing, frame-composition hook sites |
| TADR `tamem.h` | ~1,980 lines of verified structs — `UnitStruct`, `Model3DONode`, `PrimitiveStruct` |
| TADR `tafunctions.h` | Typed wrappers to call engine functions at fixed addresses |
| TADR address corpus | ~470 annotated addresses → bulk-imported as Ghidra labels |
| totala-re | 391 radare2-annotated functions → cross-imported into Ghidra |
| Patch Loader | `VirtualProtect` patching idiom; later: hex-edit-free Windows distribution via `dplayx` slot |
| petool | Only if we ever need to grow the exe itself (unlikely — we live DLL-side) |
| d3d8to9 | The vendored-translator pattern and wine-first debugging workflow, proven once already |
| TA Forever | A distribution channel if this ever ships to players |

## Phase 0 — Foothold & instrumentation

**G0 — Build & inert chain.** Fork cnc-ddraw into our repo; cross-compile `ddraw.dll` with
mingw-w64; install to the game dir; launch options `WINEDLLOVERRIDES="ddraw=n,b" %command%`.
*Exit:* game plays normally through our locally-built DLL, our build stamp in its log.
*Kill/pivot:* none — this is table stakes; if Proton refuses the override, fall back to system wine.

**G1 — Overlay proof.** Add the callback API to the fork; companion `tagpu.dll` draws a translucent
triangle + frame-time readout over the game, GL 3.3 core, in the render thread. Ini toggle.
*Exit:* overlay over a live skirmish at 60 fps, toggleable, no flicker across menus/movies.
*Retires:* context sharing, render-thread timing, the toggle discipline.

**G2 — State-read proof (the megamap dividend).** `tagpu.dll` resolves `TAmainStruct`, walks the
unit array, projects positions world→screen, draws a GPU marker glued to every unit while scrolling.
*Exit:* markers track hundreds of units through a full AI skirmish with zero crashes.
*Retires:* struct offsets on the Steam exe, camera/scroll math, per-frame read safety.

## Phase A — Own the unit pipeline (RE-heavy)

**G3 — Static map of a frame.** Ghidra project on the pristine exe; bulk-import corpus labels;
name the frame pipeline: sprite-cache build (3DO rasteriser), per-unit composite/blit, z-composite,
feature/terrain order, fog application, shadow drawing. *Exit:* a wiki note documenting the
annotated call graph of one rendered frame, with candidate hook addresses.

**G4 — Runtime confirmation.** In-process tracer in `tagpu.dll` (hit-order logging per candidate),
x32dbg under wine for the stubborn ones. *Exit:* log proving "function X draws unit N at (x,y)"
matches observed pixels; hook-site list frozen.
*Kill/pivot:* if unit pixels turn out to be drawn interleaved with terrain/features in one pass
(no isolatable per-unit composite), pivot the suppression strategy to sprite-cache substitution
(G6 becomes the permanent mechanism for Phase B, at native resolution, and we revisit).

**G5 — Suppression.** Detour the per-unit composite for one unit type; units become invisible but
remain selectable and simulated. *Exit:* 30-minute AI skirmish, stable, and the sim provably
untouched (replay/di identical, savegame byte-compare clean).

**G6 — Sprite-cache stepping stone.** For one unit type, render our GPU version of the in-memory
`Model3DONode` tree, posed from `PrimitiveStruct`, offscreen at native sprite size; read back,
palettise, write into the engine's sprite cache; the engine composites as normal.
*Exit:* A/B flip between stock and ours looks near-identical.
*Retires:* geometry decode (16.16 fixed-point), GAF texture decode, piece-tree posing — with the
engine still handling every compositing concern. *Timebox:* if cache internals fight back for more
than a week, downgrade to a side-by-side debug window (same proof, less invasive) and move on.

## Phase B — Blit-level GPU units

**G7 — One unit, truly GPU.** Suppress its blit; overlay pass draws the mesh at the exact engine
projection, posed live, palette lifted to RGB. *Exit:* the unit walks, aims, fires and dies
GPU-rendered, ≤1 px drift vs stock in A/B.

**G8 — Compositing correctness.** The hard gate: occlusion vs buildings/features, fog of war,
cloak, selection circles and health bars (drawn after units — ordering!), nanolathe effects,
water. Strategy options ranked: reuse engine z-buffer reads → stencil; else draw-order emulation.
*Exit:* curated test-scene checklist passes at "you'd leave it on" quality.

**G9 — All units at speed.** Instancing, GAF→atlas cache, cache-bypass complete; 500-unit stress
skirmish. MP-safety audit (all hooks read-only, timing perturbation bounded). *Exit = Phase 1 bar:*
full skirmish vs AI, every unit and building GPU-rendered, 60 fps, toggle away from stock.

## Phase C — Enhance the originals

**G10 — Materials & light.** Per-pixel lighting, auto-smoothed normals, true-colour palette lift,
team colour as a material channel, honest translucency, a real shadow pass. Every one of ~230 units
improves at once, zero art authoring. *Exit:* side-by-side gallery; each feature toggleable.

**G11 — Replacement pipeline.** glTF convention (piece names mirror the 3DO tree, rigid pieces,
pivots at piece origins), loader + hot reload, ONE hand-authored exemplar unit driven by live COB
state. *Exit:* the exemplar in-game; mass content explicitly out of scope — the pipeline is the product.

## Phase D — Native-resolution scene pass (started 2026-08-31)

Architecture: [native-res design](native-res-design.html), **revised for the resolution
constraint** — the GL scene pass renders at the game's REQUESTED resolution (raise it via
TA's own video options / registry, never a mixed-res hack); nothing hardcodes 640×480 — the
viewport rect `main+0x37E27..` and view dims are read live every frame.

Two static-RE agents landed at phase start (proven pattern):
- [Resolution plumbing](resolution.html) — persistence is registry-only
  (`DisplaymodeWidth/Height`, loaded UNCLAMPED, defaults 640/480), no command-line switch;
  viewport rect writer `0x4981C9..` inside game-entry `0x497F40`: **left=128 and top=32 are
  constants**, right=W−1, bottom=H−33; mode list = `EnumDisplayModes` filtered to bpp==8,
  ≥640×480, ≤100 entries — our fork serves that list, so custom modes are one enum away.
- [UI markers](ui-markers.html) — only the **selection rect** is interleaved with unit draws
  (must be re-drawn by the native pass: rotated model-XZ-bbox, 4 DrawLines, GUI colour 0xA);
  health bars, group digits, order/build markers all draw AFTER both unit sweeps (survive our
  overdraw); build-cursor draws after fog.

| Gate | Status | Evidence |
|---|---|---|
| G12a — depth scaffold proof | ● done | `tagpu_scaffold.c` live (armed by `tagpu_scaffold.on`): per-frame viewport-sized scaffold from engine data, sweep rect/dims match the engine formulas at runtime (44×58 @640×480, vp (128,32) read live), GAF silhouettes (raw+RLE decode) stamped at row-key depth, colour debug overlay + per-unit occlusion-prediction log. Empirical detour: **Painted Desert has NO tall features** (census: all real defs Height≤5) — preset skirmish moved to **Two Continents** (defs 4–16 = trees, h=40, ~4200 anchors). Live there: 47–63 tree silhouettes stamped/frame, zero fallbacks, and the first live prediction — `ARMCOM occl=26%` behind a tree. EXIT MET: live prediction `CORSOLAR occl=10% (294/2704 px)` matches the engine frame — the tree overdraws exactly that corner of the building (panel below). Bonus fixes shipped same session: movers/builders no longer flicker (per-unit pixel cache repainted synchronously inside the owndraw detour — `repaint≈100% miss=0`), spinner dropout killed by a 16-variant box cache with max-coverage hotspot-aligned fallback (CORMEX verified clean over 1200 frames at **60 fps** — 30 fps capture aliases one-present dropouts invisibly, the key debugging lesson; recipe in the repo `ta-capture` skill). The fork's `_DEBUG` FPS OSD (GDI text stamped into the 8bpp surface, beating against the engine redraw) was the flickering FPS counter — now opt-in via `tagpu_fpsosd.on`. Known accepted gap (user-approved skip): fast-swinging pieces can clip at the composite AABB edge — dies with G12c. |
| G12b — one unit truly native | ◐ core done (2026-09-01) | `tagpu_native.c`: commanders render as RGB into a game-resolution FBO composited over the frame — engine-posed geometry in live viewport coords, shared atlas + engine SHD rows lifted through the live palette (cycling-safe), REAL per-fragment occlusion vs the G12a scaffold (VERIFIED: the white AI commander walks behind a tree and its silhouette is clipped by the canopy — panel below), own 50%-black offset shadow (engine rules), true-alpha cloak path, per-fragment LOS/MAPPED fog sampling (code live; visual test needs a true-LOS skirmish), native team colours. Ownership handoff: under-construction units stay on the composite path until complete; owned units' composites are wiped (engine blits nothing — A/B toggles live via `tagpu_native.on`). **2026-09-01 late-night close-out:** SELECTION RECT native (flat model-XZ AABB from a cached whole-3DO-tree walk, rotated by body yaw, GUI colour 0xA, GL_LINES just under the unit's depth — rotates with a walking commander exactly like the engine's; A/B panel below). True-LOS skirmish LIVE (`SkirmishLineOfSight`/`SkirmishMapping` REG_DWORDs — the guessed `LineOfSight` REG_SZ name was wrong): fogMode=3, per-fragment unexplored-discard + out-of-LOS darkening in the shader, own units verified against the gradual-reveal edge. **That per-fragment source-map rule was later found to be wrong outright and replaced in G13c** — the enemy-straddling-the-boundary case is settled there (an enemy structure on grey ground is not drawn), so nothing remains here. |
| G12c — all units + 3D wrecks | ◐ core done (2026-09-01) | `tagpu_native.on` token `all`: every COMPLETE unit renders natively (buildings, mexes, windgens, floating makers — team colours, shadows, tree occlusion all correct; panels below); under-construction units hand over at completion; airborne units get the nearest depth band with ground-anchored shadows; native pass clipped to the live viewport rect (engine parity — without it units overdraw the HUD). Anchor parity vs composite: (−0.44, +0.11) game px. **2026-09-01 late-night close-out:** (a) 2× SUPERSAMPLING live for the native FBO (render at 2×, exact box downsample via LINEAR quad into the 1× FBO, NEAREST composite keeps the game-res look; visibly antialiased silhouettes, panel below; `tagpu_ss.off` kill switch). (b) NATIVE WRECK PASS CODE LIVE (`tagpu_native.on` token `wrecks`): sweep-rect FeatureStruct walk (flags bit0 + FeatureMask bit0 clear → record `*(main+0x1420B)`+idx*0x30) emits husks at feature depth 3+rel·4; the owndraw classifier now recognises the scratch fake-unit (`*(main+0x1420F)`+0x9E == obj3do mid-draw) and wipes-instead-of-restores while armed — this ALSO fixes a latent bug where `all` would have skipped a fresh husk's first rasterise with nothing cached (invisible corpse). **VERDICT CORRECTED (2026-09-02): unit & building wrecks ARE 3DO models.** The night-2 claim that "all corpses are GAF wreckage sprites" was WRONG — it read the counter, not the data. Ground truth (feature catalogue + engine draw path + a live spawned `armlab_dead`): all 371 `*_dead`/`*_heap` corpse defs have `FeatureMask` (+0xFE) **bit0 CLEAR**, and `0x46A610` sends bit0-clear features down the scratch-fake-unit → `DrawUnit 0x45AC20` **3D composite path** (bit0-SET is the GAF path). `armlab_dead`: `objects3d` (+0x98) is a valid 3DO pointer, `SeqName` (+0xAC) NULL; its live wreck record holds a real `Object3doStruct` (+0x04, NumParts=1, posed). The bit0-SET GAF features are reclaimable scenery/foliage — rocks, trees, **burnt/dead trees** (`Tree1Dead`… = foliage, not unit husks), scars, smudges, ore, map dressing. The night-2 counter read 0 for two reasons, both now fixed/known: (i) the wreck-gather read the wreck record's `+0x08/0x0C/0x10` positions as whole world units, but they are **16.16 fixed-point** → every wreck projected ~1700<<16 px off-screen and was culled (nu=0). Fixed (`>>16`); now `native: 1 wreck(s) 486 verts`. (ii) `tagpu_owndraw_init` installs its rasteriser detours ONLY if `tagpu_owndraw.on` exists at DllMain (no per-frame re-arm), so arming owndraw AFTER launch never suppressed the engine — the pass was never actually exercised. Arm owndraw + native BEFORE launch (`--restart` preserves `*.on` triggers). **3D-WRECK A/B PRODUCED (2026-09-02):** `armlab_dead` on Painted Desert renders GPU-side matching the engine silhouette; the 8bpp engine frame with our pass armed shows no wreck body (only the cached shadow) = every wreck pixel is ours. (c) army stress: measured escalating 23→31→35 native units, 9 372→15 969 verts, 60.0 fps THROUGHOUT at 1024×768 with ss=2 (2048×1536 internal FBO) at live AI bases/battles; vertex budget projects ~100 units ≈ 45 600 verts, inside the 49 152 cap. Open: factory-cargo layering (accepted). Shadow ownership SETTLED 2026-09-02 (panel above; shadows-cloak.md): no double shadows — the engine keeps its cached slant shadow on structures/wrecks, we draw the composite-derived silhouette for mobile units only, FBI noshadow/canhover/floater honoured; factory-built units verified to clear the structure bit. Waterline/digger clipping implemented (tint own, erase unseen enemy, cut shadows; panel above). |
| G12d — freedoms | ◐ sub-pixel done (2026-09-01) | **SUB-PIXEL MOTION VERIFIED at present rate**, and better than planned: the engine already stores 16.16 fixed-point positions (unit+0x6A/6E/72 — the roster "shorts" at +0x6C/70/74 are just their high words; spotted via DrawUnitSelectBoxRect's operands in ui-markers). The native pass reads the true fractions and interpolates between the last two sim samples per unit slot (distance-snap guards slot reuse; `tagpu_subpix.off` kill switch). Numeric filmstrip via `tagpu_spxlog.on` (60 Hz anchor log): subpix ON advances ~0.33 px EVERY present frame (e.g. 393.345→393.944→394.542 across one 1.2 px sim step); OFF shows the raw 30 Hz staircase (460.019, 460.019, 460.814, 460.814 — one step every second frame). **Night 2:** HI-RES REPLACEMENT SLOT PROVEN — `gamedir/hires/<UnitName>.obj` (v/f/c subset, palette-index colours) renders INSTEAD of the 3DO: engine anchor + body yaw + our LUT/palette shade pipeline, hot reload on mtime (~2 s iteration loop). A smooth 210-tri hemispherical dome replaced the solar collector — visible curvature shading the 1997 rasteriser cannot do (panel below). Feeds G11 directly (the loader slot + anchoring exist; G11 adds glTF + per-piece COB pose). Palette lesson: flat colours go through the SHD LUT — only indices with sane ramps work (GUI colours render black); swatch-strip test mesh maps the palette in one capture. SMOOTH-ZOOM PROTOTYPE demoed at 2×: native units scale about the view centre (uZoom in the VS + inverse transform for the scaffold lookup), terrain/UI stay 1× — honest verdict: reads as a floating-units demo, exactly as predicted; full zoom needs G13 terrain. Remaining freedom: cloak polish (needs a cloakable unit) — tracked with the rest in [GPU status, hooks & limits](gpu-status.html) §3. |
| G12e — effects pass (weapon fire, explosions, debris) | ● done (2026-09-02) | The engine's two effects passes are reverse-engineered ([Effects](effects.html)) and replaced: `tagpu_fx.c` gathers `ProjectileStruct[]` / `ExplosionStruct[]` / the debris particle slots and draws lasers and lightning as lines, rockets/missiles/shells and debris as 3DO models through the native geometry path, sprite weapons, flares, explosions and the LHT light flash as sprites from a private RLE-decoding GAF atlas — at the engine's depth band (above every ground row, below aircraft), LOS-gated per projectile like the engine. `tagpu_fxown.c` owns the draw: two call-site redirects + four leaf detours, the skip following `tagpu_fx.on` live; tacli auto-arms it at launch. Verified same-fight (`passive` token = engine draws while we log): laser colours palette-identical, EMG sprites, rockets with thrust flames, explosion sprites + flashes, flying debris; the engine surface shows no effects while ours draw. Not reproduced: the rendertype-2 refraction ball; smoke/fire/wake particles stay engine-side (hook-vector sfx — next gate). |
| G12f — particle sfx (smoke, fire, wakes, nanolathe) | ● done (2026-09-02) | The ten `0x471F90(ctx, n)` sites in `DrawGameScreen` — filed in terrain-depth.md as "plugin-layer hooks, empty in stock play" — are the particle sfx: pooled 76-byte objects in ten *layer* vectors at `*(main+0x38D77)`, each layer drawn at a fixed depth of the frame (2 = wake foam under everything, 4 = feature smoke, 5 = rocket-trail puffs, 6 = nanolathe spray, 7 = bubbles, 9 = impact/damage smoke and fire over everything), the layer being the emitter's argument ([Effects](effects.html) §7). `tagpu_sfx.c` walks the layers, classifies each object by vtable (Smoke1/Smoke2/fire/flare = alpha sequence sprites; wake/nano = 2×2 `DrawBar` dots) and emits through the effects buckets with per-layer depth keys derived from the frame's row count; `tagpu_fxown.c` owns the draw with one more byte-matched prologue detour on the walker (`ret 8`), its own skip byte following `tagpu_sfx.on` live. Verified same-fight on `scenarios/sfx-strait.json` (Anteer Strait): wake dots, nano spray, damage smoke and burning debris identical engine vs ours, the engine surface clean while owned; 200v200 at sim +3 with every pass live holds ~60 fps (28 sixty-frame log lines in 30 s, 217 native units and ~230 particle sprites on screen). Fixture lessons (UI repair order, metal storage, boat pathing) in the ta-drive skill. |
| G13a — features native (trees, rocks, splats, wreckage) | ● done (2026-09-02) | The last colour-keyed sprites in the 8bpp frame besides the terrain. One leaf draws every feature pixel — `0x46A610(ctx, tile, tileX, tileY)`, `stdcall` `ret 0x10`, three call sites, three bodies (3D wreck via `DrawUnit` on the scratch feature-unit, animated GAF wreck, normal feature: shadow then body, static frame 0 or the anim state, plain copy or 50 % alpha per mask bits 2/3) — and its decompile settled the two open questions the handoff carried: the leaf mutates nothing the sim reads (bodies 2 and 3 write nothing at all; body 1 only the draw-side scratch unit) and `GAFGetCurrentFramePtrAddr` is a pure read, so animation is driven by the tick and survives ownership ([Features](features.html)). `tagpu_feat.c` walks the engine's own clamped sweep rect and draws the same frames **with depth writes on** at the row key the painter's order implies (tall bodies `3 + rel*4` above that row's units at `1 + rel*4`, flat ones in a band between the particle layers the engine draws around the pre-pass, shadows a notch below with depth writes off); `tagpu_featown.c` owns the draw with one 5-byte prologue detour, gated on `native.on` carrying `wrecks` because body 1 draws husks through `DrawUnit`. **This retires the G12a scaffold as the occluder**: occlusion was verified against the engine's own draw (engine units + engine features vs ours, `scaffold.on` disarmed) and matches unit for unit; the engine surface is empty of features while owned; fog, map scrolling a 200v200 stress at ~60 fps and map scrolling all clean. Two shared modules fell out, closing three old review follow-ups: `tagpu_gaf.c` (the RLE decoder + shelf atlas, was a private copy in `tagpu_fx.c`) and `tagpu_detour.c` (the stub/patch machinery, now shared by `fxown` and `featown`). Gaps: **fog was not at parity** — the rule all four native passes shared drew over cells the engine paints black, which this pass was simply the first to make obvious; it predated G13a and got its own gate, **G13c below, which closed it** ([Features](features.html) §9); the animated-GAF-wreck body is unreachable with stock content (every `*_dead` def takes the 3D path); feature shadows are a true 50 % RGB blend where the engine does a palette-space ALP remap, so they dither differently. |
| G13b — terrain native, and the composite inverts | ● done (2026-09-02, reviewed) | The last engine-drawn world layer. `tagpu_terr.c` reproduces `0x483FA0` — a grid blit and nothing else: one `GL_R8` atlas of 32×32 cells, 64 per row, **built once per map** (`LoadMap` builds `TILE_SET` and nothing changes it; 2048×2560 for Two Continents' 5062 tiles, and a `GL_TEXTURE_2D_ARRAY` is not viable against the usual 2048-layer cap — G13h later put the cells on a 34-texel pitch with a replicated border, 2176×2720), one quad per visible cell at depth key `0.10` — under the flat-feature band and the low particle layers, i.e. the frame's implicit far plane. Water animates for free (palette cycling, and the pass already re-uploads the live palette). The engine's truncating `sar 5` and its `ceil` column count are reproduced deliberately, with a comment saying so. **Parity is exact**: 0 differing pixels against the engine's own blit in every terrain-only band, at `frac=(0,0)`, `(6,8)` and `(25,31)`. **The gate was the compositing model, and it inverts.** Terrain covers the whole viewport, so the old rule ("discard our empty pixels, let the engine's frame show") would hide health bars, nanoframe wireframes, the build cursor, chat and dialogs. `tagpu_terrown.c` therefore does not just skip `0x483FA0` — its skip path (a new `tagpu_detour_leaf_call`, nine stolen bytes) **fills the viewport rect of the engine's offscreen with one palette index, the KEY**, which also restores the clear that the terrain repaint used to provide; the composite then discards *our* fragment wherever the engine's frame is not the key, reading it straight out of cnc-ddraw's own `R8` index texture with `texelFetch` (`TAGPU_FRAME.surface_tex`). The key is **254**, not a guess: the engine's GUI colour table at `main+0xDCB` uses `0xFD` and `0xFF` and leaves `0xFE` in the gap. The **fog overlay `0x4848E0` is suppressed with the terrain** (its shade remap would rewrite the key into grey blobs, and we have reproduced it since G13c) **with its lazy grid rebuild replicated exactly**, because our own fog rule samples that grid. Terrain is the bottom layer now, so it paints the fog's solid black rather than discarding (`TAGPU_GLSL_FOG_TERRAIN`). Verified: engine surface **99.3–99.98 % key** with no terrain left; engine chat, `PAUSED`, selection boxes, the build panel and the cursor all survive inside the viewport; fog **99.06–99.39 %** lit-vs-grey agreement (99.48 % of pixels identical at a fully-fogged corner, mean abs 0.32/255) with only the dithered edge differing; in-process map change rebuilds the atlas (5062 → 7051 tiles); map corners clean with `off-map=0`; `terr.on=off` and the 90-frame watchdog both restore the engine's terrain *and* fog; 200v200 with everything native at **59.7 fps**. It also fixed a G13c gate bug it made fatal: `fogMode` bit0 was `LosType & 1`, the **mapping** option, so true-LOS-without-mapping (`LosType=14`) skipped the fog rule entirely — invisible while the engine drew its own overlay, a missing grey band once we suppress it. Review closed the three failure modes an inverted composite has and an overlay does not (emitting without owning; the disarm frame; a screen that never calls `0x483FA0`) — all written up with their symptoms in §7.6, along with the one review finding that was **wrong** and must not be "fixed" later. [terrain & depth](terrain-depth.html) §7. |
| G13c — fog of war at parity | ● done (2026-09-02) | **Reverses [terrain & depth](terrain-depth.html) §6 item 4.** All four native passes mirrored fog by sampling the LOS/MAPPED source maps per fragment; that advice was wrong and the gate proved it. The engine's overlay `0x4848E0` is driven entirely by the view-anchored corner-mask grid `0x4843C0` builds behind `*(main+0x1421F)`, and the source maps cannot reproduce it: the lattice is offset **half a cell** (a grid corner sits at a map cell's *centre*, `origin = 32·col0 + 16`), its shape is a **4-bit corner mask** feathered by 14 GAF edge sprites that a per-cell boolean cannot express, and — read in the *same frame* at the same cells — MAPPED reported explored across a band the engine paints solid black (that last discrepancy is still unexplained, written up in terrain-depth §5.2; it is moot for rendering because the grid is by construction what the engine drew). The fix is one shared rule in `tagpu_glsl.h` replacing four copy-pasted blocks and the `uLos`/`uMap` pair with a single `uFogGrid`: the grid uploads as an **RG8 texture with no conversion** (its two bytes per cell already *are* the unexplored and out-of-LOS masks), and **bilinear coverage over the four corner bits thresholded at 0.5** reproduces the 14 edge shapes — `0xF` → everywhere, `0x3` → exactly the top half, a lone corner → its quadrant — clean where the engine dithers. The grey darken is the engine's own shade LUT `*(TAProgram+0xCC)` applied to the palette **index** before the palette fetch (a multiply on the resolved colour costs 19,768 mismatched px — visibly too dark on canopies). **Units and effects hide in grey while terrain, features and wreckage stay and are remapped** — the rule the passes did not implement at all before. MEASURED, `feat-forest` on Two Continents, GL framebuffer, engine draw vs ours at the same camera: mapping-only 48 px lit only in the engine's / 821 only in ours / 287,531 agreeing; true LOS **97 / 186 / 576,990** (0.05 %). Verified live that an enemy solar collector on fully-grey ground is **not drawn** while the flattened building footprint it stamped into the terrain still shows — TA's own "grey shows terrain but not units". Panel and method: [Features](features.html) §9. |
| Resolution track | ● done (2026-09-01 night 2) | LIVE AT 1024×768: registry `DisplaymodeWidth/Height` (REG_DWORD 1024/768) → the game runs the mode; every live read matched the RE formulas exactly — vp=(128,32), view=896×704 (=W−128, H−64), sweep=68×76, native FBO 1024×768 (ss=2 ⇒ 2048×1536 internally); UI lays out fine (top-bar art tiles on the right — acceptable); clicks/camera/build all worked with ZERO tool recalibration (the in-process driver computes from live vp/game dims by construction; only capture-crop scale changes: 2.8125 vs 4.5). THE REAL PAYOFF: the first in-game mode switch ever exercised exposed that `dd_SetDisplayMode` restarts the render thread with a NEW GL CONTEXT — all our cached GL ids die silently (stale-FBO bind → our pass cleared the real backbuffer black). Fixed with context-change detection (wglGetCurrentContext each frame) + per-module glreset (state, texture dims, AND the CPU-side atlas/LUT upload caches — forgetting those left everything sampling black). Mode switches are now robust — a G13 prerequisite banked early. |

## Phase D wrap-up & the road beyond (PROPOSAL, 2026-09-01 — for review)

With G12a/b/c landing, the native pass owns every complete unit's pixels. Proposed order of
what remains, cheapest-win-first; each row is a discussable unit of work:

| # | Work | Why now / exit |
|---|---|---|
| 1 | **G12c close-out**: native wreck rendering (wreck records `*(main+0x1420B)`, opportunistic once a war leaves 3D husks), army-scale stress (fps + vertex budget at 100+ on-screen units, sim +10), 2× supersampled edges for the native FBO (visual parity with the composite path's SS) | full skirmish, no 8bpp unit pixels, 60 fps |
| 2 | **G12b leftovers**: ~~fog A/B on a LineOfSight=true skirmish~~ (**done, G13c** — `LineOfSight` cycle stage 1 gives `LosType=15`/`fogMode=3`; the A/B is in [Features](features.html) §9); selection rect + health-bar reorder decisions (needs clicks → unlocked session) | native unit darkens at a fog edge exactly where the engine's 32-px cells would |
| 3 | **Resolution live test** (the constraint's payoff): registry `DisplaymodeWidth/Height` → e.g. 1024×768 from our fork's mode list → verify viewport rect/native pass/scaffold at the new mode; recalibrate session tooling; empirically check the flagged HUD-art risks | the same skirmish, playable at a higher requested resolution, all passes correct |
| 4 | **G12d freedoms**: sub-pixel motion (render-side interpolation of integer engine positions), true-alpha cloak polish, first **hi-res model experiment** (feeds G11's glTF pipeline), smooth-zoom prototype on the ortho transform | one visibly-better-than-1997 clip per freedom |
| 5 | **G11 — replacement pipeline**: glTF convention + loader + ONE exemplar unit driven by live COB state | the exemplar in-game |
| 6 | **G13 — full scene takeover**: ~~features → GL sprites~~ (done, G13a — and they write the depth the scaffold used to fake), ~~terrain tiles → GL~~ (done, G13b — TNT tile atlas, and the composite inverted with it), ~~projectiles/explosions native~~ (done, G12e), ~~particle sfx native~~ (done, G12f), ~~fog native~~ (done, G13b — the overlay is suppressed and ours replaces it), ~~health bars / order markers / build cursor~~ (done, G13d — re-drawn and captured-and-replayed), ~~free zoom~~ (done, G13e — the world, the click, the cursor, the minimap box and the scroll rate all scale together); UI/minimap stay engine-side by design | ✅ engine software frame = UI only, and the zoom it was the entry condition for is finished |
| 7 | **Cross-cutting, ongoing**: MP-safety formal replay byte-diff (unlocked session), packaging/distribution story (drop-in ddraw.dll + config), TADR-chain coexistence check | — |

**G13 — Full-frame ownership.** The engine's software frame becomes data only (GUI/minimap still
sampled from it); we draw terrain, features, units and effects; ortho + free zoom; assess
coexistence with the TADR chain for a distributable build.

## Standing rules

- **Preservation:** nothing in the Steam dir is ever modified — only added. `pristine/manifest.md5`
  is the tripwire; Steam Verify is the restore.
- **Documentation:** every RE finding lands in this wiki as it's made, with addresses, so the work
  compounds the way the community corpus did.
- **Honesty about provenance:** vendored knowledge (TADR, totala-re) stays attributed, MIT terms kept.
