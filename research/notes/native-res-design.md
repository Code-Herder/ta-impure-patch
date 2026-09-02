# Native-resolution scene pass — design note (G12)

*The architecture for rendering units past the 8bpp composite ceiling: directly into the GL
frame — true colour, translucency, per-fragment fog, and eventually smooth zoom. Written
2026-08-31 at Phase C start from the [terrain & depth](terrain-depth.html) RE (which see for
every address cited); **revised 2026-08-31 at Phase D start for the resolution constraint
(§0)** — the pass renders at the game's requested resolution, never at an invented one.*

## 0. THE RESOLUTION CONSTRAINT (user-stated, governs the whole phase)

> Resolution must respect the requested window resolution. If we want higher res, go into
> video options and increase res (or command line param).

What this means for the design:

1. **The GL scene pass renders at the game's requested resolution** — whatever TA is set
   to via its own video options (or a command-line/registry path). NOT at display
   resolution, NOT at an invented internal resolution, and never as a mixed-resolution
   hack over a 640×480 frame. Concretely: a game-resolution RGBA+depth FBO, composited
   over the engine frame with the same letterbox transform cnc-ddraw already applies —
   one uniform scale for the whole picture.
2. **Higher fidelity = raise TA's own resolution.** cnc-ddraw (our fork) serves the mode
   list through `EnumDisplayModes` and can add custom modes — we control both sides. The
   win of the native pass at any resolution is true colour, translucency, per-fragment
   fog, depth-tested occlusion and sub-pixel freedom; the win of more pixels comes from
   TA's video options.
3. **Nothing hardcodes 640×480 — or any dimension.** The projection's `+128/+32` offsets
   ARE the engine's viewport rect, live at `main+0x37E27..0x37E33` (left=128, top=32,
   right, bottom at the stock resolution; `main = *(0x511DE8)`); view W/H px at
   `main+0x37E37/0x37E3B`; view dims in tiles at `main+0x1423B/0x1423F` (16px) and
   `main+0x14243/0x14247` (32px). **Read them every frame**; treat every constant in the
   older notes (128, 32, 640, 480) as "the value that rect happens to hold at 640×480".
4. **RE prerequisite (parallel track):** how TA sets/persists resolution — the video-
   options flow from `SetDisplayMode` caller `0x4B563C`, where width/height land in the
   main struct, how the viewport rect is derived from them, and whether a registry value
   or command-line switch exists. → `research/notes/resolution.md`.
5. **Session tooling breaks at non-640×480:** the xdotool content-rect calibration
   (desktop x[1560..4440], scale 4.49) and all crop math assume 640×480 fullscreen —
   recalibrate after any resolution change (self-calibrate against the memory-read mouse,
   as in G2).

## The one-paragraph idea

Keep TA's software frame for everything that is not a unit (terrain, features, fog, UI —
all engine-drawn, upscaled by cnc-ddraw as today), but remove *unit pixels* from it and
draw units ourselves in the GL present hook, at the game's requested resolution, 1:1 with
the engine offscreen, depth-tested against a **scene-depth scaffold we synthesise from
engine data**. The engine has no screen depth plane ([terrain & depth §4](terrain-depth.html))
— its ordering is a painter's sweep keyed on 16-px map rows — so we reproduce that key as
a GL depth scalar and get stock-correct occlusion with per-pixel freedom inside it.

## Why the composite path can't get us there

The 8bpp composite (Phase B) is palette-locked: 256 colours, no translucency, sprite-sized,
integer-anchored. Everything else about it is perfect (engine does binning/fog/UI/minimap)
— which is exactly why the native path must re-create those services for unit pixels only:

| Service | Engine gives the composite path | Native pass must |
|---|---|---|
| Occlusion vs tall features | painter's row sweep | depth scaffold (below) |
| Fog of war | fog pass remaps unit pixels after draw | per-fragment fog from the engine's **fog grid** (G13c; the LOS maps were tried first and do not reproduce the overlay) |
| Shadows | silhouette alpha-blit inside blit 0x459200 | re-create (shadows-cloak.html rules) |
| Cloak | ALP 50/50 blend on the blit path | true alpha (an upgrade, G12d) |
| Selection circles / health bars | drawn over units in 8bpp | draw our own (or accept them under us, stage 1) |
| Minimap/radar | TNT picture + radar dots, not unit sprites | nothing (unaffected) |

## The depth scaffold (the core trick)

Per frame, from live engine data, build a depth texture covering the viewport rect (read
live, §0.3):

- **Terrain** = far plane. Engine-faithful: terrain never occludes a unit.
- **Flat features** (def Height<10, `FeatureDef+0xFA`) = far plane too (they draw under
  everything in the engine's pre-pass).
- **Tall features**: blit each feature's GAF silhouette (defs at `*(main+0x1426F)`, stride
  0x100; anchor row from `PLOT_MEMORY *(main+0x14287)`) into the scaffold at depth
  `rowKey = anchorRow`, colour-keyed. ~dozens of sprites/frame, trivial for GL.
- **Units** (ours): fragment depth = `unitRowKey` primary (`floor(unit+0x74 / 16)`),
  refined *within* the unit by our per-pixel model depth (the FBO already produces it).
  Tie-break rule from the sweep: within a row, units draw before tall features → bias unit
  depth slightly farther than the same row's features (scalar `rowKey*2 + isFeature`).
- **Airborne units** ((state&3)!=1): a nearer band above all ground rows (engine draws
  them in a second sweep after projectiles).

Projectiles/explosions stay engine-side in the 8bpp frame (they'll draw *under* our units;
acceptable at stage 1 — revisit when effects go native).

## Frame timing & hooks

All reads happen in the present hook we already own (pre-SwapBuffers in `render_ogl.c`),
which runs at the Lock/Unlock heartbeat *after* `DrawGameScreen` — unit state, eye and LOS
maps are exactly the frame being shown. No new engine hooks needed for stage 1 except the
**unit-pixel removal**: the owndraw detours stay (engine keeps pose-sync/AABB/alloc), and
we simply stop writing the composite colour plane back for natively-drawn units — the
engine blits a ColorKey-empty plane, a no-op. Fog, UI, dialogs, present: untouched.

World→screen in engine coordinates (all live reads, §0.3):
`sx = wx − eyeX + vpLeft`, `sy = wz − alt/2 − eyeY + vpTop`
(eye at `main+0x1431F/0x14323`, vp rect `main+0x37E27..`). The native pass rasterises at
these coordinates into its game-resolution FBO; cnc-ddraw's letterbox transform then
scales the composed frame to the display exactly as it scales the engine frame today.

## Fog per fragment

**SUPERSEDED 2026-09-02 by G13c — the plan below was implemented and is wrong.** It read:
~~visible = the LOS counter byte map (`main+0x1B63 + localId·0x14B + 0x7C`), explored =
`*(main+0x14273)` u16 bit `localId`, sampled per fragment; smooth edges come free.~~
Sampling the *source* maps does not reproduce the engine's overlay: the overlay is driven
by the view-anchored corner-mask grid behind `*(main+0x1421F)`, whose lattice is offset
half a cell from the map cells and whose shape is a 4-bit corner mask, and the source maps
were measured reporting "explored" across a band the engine paints solid black.

What the passes actually do now (one shared rule, `tagpu_glsl.h`): upload the grid as an
RG8 texture — its two bytes per cell *are* `(unexplored mask, out-of-LOS mask)` — and take
bilinear coverage over the four corner bits, thresholded at 0.5. Smooth edges still come
free, and they land where the engine's do. The explored-dark darken **is** the fog LUT at
`*(TAProgram+0xCC)`, applied to the palette index (a plain factor is visibly too dark).
Units and effects are *hidden* in grey, not darkened. Full write-up:
[features §9](features.html); geometry in [terrain & depth §5.2](terrain-depth.html).

## Staging (each stage is a live demo)

1. **G12a — depth scaffold proof.** Build the scaffold each frame at the game's requested
   resolution; debug-visualise it as a colour overlay (row gradient + feature
   silhouettes). No unit rendering change. *Exit:* the overlay predicts engine occlusion
   in live scenes.
2. **G12b — one unit truly native.** Commander: stop the composite write-back for it
   (owndraw stays), render RGB in the present hook at game resolution — palette lift +
   SHD shading + build-state scaffold — depth-tested vs G12a, fog per fragment.
   Re-create its shadow (shadows-cloak.html rules) and cloak alpha. A/B toggle.
   *Exit:* walks behind a tall feature, darkens under fog.
3. **G12c — all units + 3D wrecks** (wreck records `*(main+0x1420B)` stride 0x30, live
   `Object3doStruct*` at +4 — our existing 3DO walk renders them). *Exit:* full skirmish,
   no 8bpp unit pixels.
4. **G12d — the freedoms.** True translucency (real cloak), sub-pixel motion, hi-res
   model experiments, smooth-zoom experiments on the ortho camera.
5. **Resolution track (parallel with G12a):** the §0.4 RE + a live test — raise TA to a
   higher mode via video options (mode list comes from our fork), confirm the viewport
   rect and roster projections update, recalibrate the xdotool math (§0.5).

## Known gaps & kill/pivot rules

- **Selection/health bars under our units** (engine draws them into the 8bpp frame before
  we overdraw): stage-1 accept; fix = draw our own from unit state, or reorder — RE track
  in `research/notes/ui-markers.md`. If it reads terribly, pivot: render units into the
  8bpp frame at native positions but palette colours (half-step).
- **Projectiles under units**: acceptable jank at stage 1; effects go native later.
- **Build-state**: the composite path's scaffold is blit-time
  ([build-state](build-state.html)) — the native pass implements its own scaffold look
  (we already own a 5-stage FS scaffold from Phase C; port it, now free of the palette).
- **Shadows**: engine unit shadows live inside the composite blit — natively-drawn units
  lose them; re-create per shadows-cloak.html (blackened silhouette, options-gated
  `main+0x37F06 & 0x10`).
- **MP safety**: unchanged — all reads, render-only skips, same as Phase B.

## Non-goals (this phase)

Terrain/feature GL rendering (that's G13 full takeover), minimap changes, replays/savegame
formats, any sim-side write, any resolution not requested through TA's own configuration.
