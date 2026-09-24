# BAR's camera and a full-colour Classic — the port plan

**Written** 2026-09-23. **C1–C4 of Part 1 are built (G20a, on its worktree branch, not landed);
C5 and Part 2 are not.** It is the plan for two changes the lab has already made:

1. **Part 1** covers the camera's pan and zoom. **The BAR camera replaces the game's camera. There
   is one camera and no setting to choose another.** Beyond All Reason's centre clamp, zoom-out
   from the centre, notch and ease go into `tagpu`, and the rules they replace are deleted. The
   lab's mirrored map edge follows as an on/off setting (C5, G20b). The projection (TA's fixed
   oblique view) does not change.
2. **Part 2** covers the renderer. Classic becomes the Classic++ full-colour pipeline with its
   rendering options off, and the 8bpp path is deleted.

The reference implementation for both is `tools/tascene-view.html` on `worktree-camera_zoom`
(4171779..b2574e4). At `b2574e4` its **BAR** button in the status bar was `cam=bar&edge=mirror`,
and that page is what the owner has looked at; since G20a the lab has that camera alone, and
`edge` is a status-bar toggle. Where BAR and the lab disagree, the lab wins, and the difference
is written down. When a gate here closes, its facts move to [GPU status](gpu-status.html) and
[renderers](renderers.html), and this note is folded into them the way `g19f-plan.md` is.

Evidence tags as elsewhere: **[SOURCE]** read from the code named, **[MEASURED]** with the
numbers, **[INFERRED]** not established, **[DECIDED]** the owner's call with a date, **[OPEN]**
not settled.

---

## Part 1 — The BAR camera: pan and zoom only

**The scope [DECIDED 2026-09-23].** This part changes only **how the camera pans and zooms**.
It covers BAR's options 1 and 2 (the centre clamp and zoom-out from the centre), BAR's wheel
notch and ease, and option 3, the mirrored map edge that the centre clamp brings into view.

**One camera [DECIDED 2026-09-23].** The BAR camera **is** the game's camera from G20a on.
The rules it replaces are **stripped, not kept behind a switch**:

- the window clamp;
- zoom-out about the pointer;
- the ×1.1 notch and its log ease;
- the `tagpu_zoomedge.off` escape hatch, which puts the 1× range back.

There is no `camera` setting, no lever and no fallback (*strip fully, no compat*). The only
control that shows anything else is the stock engine itself: an instance with the zoom pass
unarmed (`tagpu_defaults.off`) runs TA's own 1× camera, as it does today.

**TA's projection does not change.** There is no tilt, no rotation, no perspective and no FOV. The
camera is still TA's fixed oblique view, and every value below is BAR's Spring camera's value for
pan or zoom only.

### 1.1 The lab's BAR preset is the target  [SOURCE: `tools/tascene-view.html`]

In the lab at `b2574e4`, `cam=bar` set BAR's camera rules at once and the BAR button added
`edge=mirror`. The table is the change: the left column is **what G20a deletes**, the right is
what replaces it.

| rule | the game today — **deleted** | BAR — **the game's only camera** |
|---|---|---|
| **clamp** (how far it pans) | `window`: eye ∈ [−d, map − W + d], where d = ⌊W/2 · (1 − 1/z)⌉ above 1× and 0 at or below it (`zoom_eye_range`). Zoomed out, the map edge stops W/2 from the screen centre | `centre`: the world point at the view centre stays on the map at **every** zoom, so eye ∈ [−W/2, map − W/2]. The map's edge can reach the middle of the screen |
| **zoom in** | holds the world point under the pointer | the same |
| **zoom out** | holds the world point under the pointer | `centre`: the camera pulls straight back, and the view centre holds |
| **notch** | ×1.1 on a target *level*, snapped to exactly 1 inside (0.999, 1.001) | ×(1 − 0.14 n) on the camera *distance*, which is 1/z. That is ×1.163 in and ×0.877 out, so in-then-out lands on 1.020, not 1 |
| **ease** | each presented frame covers a quarter of the remaining log-distance and snaps at 0.0025. A notch is 82 % done in 6 frames and lands in 13, so its length follows the present rate (0.22 s at 60 Hz) | a 250 ms tween from the **drawn** pose to the new target along g = 1 − (1 − f)⁴. Centre and 1/z are lerped in straight lines. 24 % of the notch lands in the first 60 Hz frame and 80 % by 83 ms. Its length is the same at any frame rate |
| **edge** | black | `mirror` (§1.4) |
| **range** | 0.25..8 | 0.25..8 in the lab too. BAR's own range depends on the map (§1.2) |
| **scroll rate** | the same on screen at every zoom (`apply_scroll_rate` scales `ScrollSpeed` by 1/z) | unchanged. BAR's is the same property (§1.2) |

The notch counts from 0.25× to 8× are 36 for the game, 23 in and 27 out for BAR.

**A pointer off the world viewport does not zoom at all**: the game takes the wheel only over
the world viewport, so a notch over the side panel, the bars or a dialog is not consumed
(`tagpu_zoom_wheel`; *"wheel ignored — pointer is off the world viewport"*). The lab has no such
region. A second notch during a tween starts a new tween from wherever the camera is drawn, toward
a target built from the **previous target**. Recoil does the same:
`CameraTransitionExpDecay` takes `startPos` from the drawn camera, and the controller's pose has
already moved.

### 1.2 BAR's pan and zoom values, and where they come from  [SOURCE]

Read on 2026-09-23 from three repositories:

* [RecoilEngine](https://github.com/beyond-all-reason/RecoilEngine) `e9d1993`: the engine's
  `CONFIG` defaults and `rts/Game/Camera/SpringController.cpp`, which is the camera BAR plays in.
* [Beyond-All-Reason](https://github.com/beyond-all-reason/Beyond-All-Reason) `c958cfa`: the game.
  `luaintro/springconfig.lua` writes values at startup and `luaui/Widgets/gui_options.lua` holds
  the options menu.
* [BYAR-Chobby](https://github.com/beyond-all-reason/BYAR-Chobby) `bdfae3c`: the lobby, which
  ships `LuaMenu/configs/gameConfig/byar/defaultSettings/springsettings.cfg`.

**A BAR player runs the engine default unless one of the last two overrides it**, so the
effective column is the one to read.

#### Zoom

| setting | engine default | BAR effective | what it does |
|---|---|---|---|
| `ScrollWheelSpeed` | −25 | **−20** (Chobby) | the wheel's `move = notches × ScrollWheelSpeed` (`MouseHandler.cpp:665`). Negative means wheel-up zooms in |
| the zoom step | — | `scaledMove = 1 + move × 0.007` (`SpringController.cpp:199`) | the camera distance is multiplied by this. At −20 that is **0.86 per notch in and 1.14 out** |
| `CamSpringZoomInToMousePos` | true | true | zoom in moves the camera along the ray through the pointer by `1 − scaledMove` of the distance to the ground, never past `minDist` (`ZoomIn`). The point under the pointer holds |
| `CamSpringZoomOutFromMousePos` | **false** | false (the menu's `zoomfromcursor` row) | with it false, `ZoomOut` returns with the focus point unmoved, so zoom-out pulls back from the centre |
| `CamSpringMinZoomDistance` | 20 | **300** (`springconfig.lua` v5; the menu's `mincamheight` slider, 0..1500) | the closest zoom, in elmos along the view ray |
| max distance | — | `1.333 × max(mapx, mapy) × 8` elmos (`SpringController.cpp:49`) | the farthest zoom: the map's longer side and a third more. It depends on the map; the game's 0.25× floor does not |
| `CamSpringFastScaleMousewheelMove` × `CameraMoveFastMult` | 0.2 × 10 | the same | Shift on the wheel: `shiftSpeed = 2`, so a Shift notch in is **×0.72** |

#### The ease

| setting | engine default | BAR effective | what it does |
|---|---|---|---|
| `CamTransitionMode` | **0, exponential decay** | 0 | the menu's `smoothingmode` offers 1 (spring-damped) as its alternative |
| the wheel's transition length | — | **0.25 s** (`SpringController.cpp:214`) | `camHandler->CameraTransition(0.25f)` on every notch |
| `CamTimeFactor` | 1.0 | 1.0 (Chobby) | multiplies the length |
| `CamTimeExponent` | 4.0 | 4.0 (Chobby) | `tweenFact = 1 − timeRatio^4`, with `timeRatio` running from 1 to 0 (`CameraHandler.cpp:419`), which is g = 1 − (1 − f)⁴, an **ease-out** |
| `CamFrameTimeCorrection` | 0 | 0 | the tween measures from `lastFrameStart` |
| `CamSpringHalflife` | 100 ms | 100. The menu's `camerasmoothness` slider (0.18 by default, 0.04..2) writes `0.18 × 200 = 36 ms` **only when the player moves it** | used only by the spring-damped modes, so it is inert under mode 0 |

#### Pan

| setting | engine default | BAR effective | what it does |
|---|---|---|---|
| the clamp | — | `pos.x ∈ [0.01, mapx·8 − 0.01]`, `pos.z` likewise, in `Update()` (`SpringController.cpp:394`) | **the focus point — the view centre on the ground — is clamped to the map**, and nothing else is. This is the centre clamp |
| `CamSpringScrollSpeed` | 10 | 10 (the menu's `cameramovespeed`, 0..100) | key and edge scroll: `pos += dir × pixelSize × 2 × scrollSpeed`, and `pixelSize` scales with distance, so **the screen moves at one rate at every zoom**, as TA's does |
| `CameraMoveFastMult` / `SlowMult` | 10 / 0.1 | the same | Shift / Ctrl on scrolling |
| `WindowedEdgeMove` / `FullscreenEdgeMove` | true / true | true (the menu's `screenedgemove`) | |
| `EdgeMoveWidth` | 0.02 | **0.003** (Chobby) | the band as a fraction of the window: 5 px at 1920 wide (`max(1, ⌊W × w⌋)`). TA scrolls on the exact edge pixel |
| `EdgeMoveDynamic` | true | **false** (Chobby) | constant speed inside the band, not faded |
| `MiddleClickScrollSpeed` | 0.01 | **−0.001** (Chobby; the menu slider spans −0.01..−0.00195) | middle-drag pan. Negative means the view follows the drag |
| `MouseDragScrollThreshold` | 0.3 s | 0.3 (the menu's `middleclicktoggle`) | a middle click shorter than this toggles locked scroll mode |
| `CamSpringFastScaleMouseMove` | 0.3 | 0.3 | Shift on middle-drag |

#### BAR's Map Edge Extension (`luaui/Widgets/map_edge_extension2.lua`)

| value | BAR | the lab's `edge=mirror` (what is ported) |
|---|---|---|
| source | the **minimap texture**, mirrored, on the mirrored heightmap | the map's own **tiles**, texel for texel, with each texel flipped across every edge it is past; plus the map's own **features** |
| tone | `brightness = 0.3` on the luma (Y of YCbCr), chroma kept | `edgedim = 0.5` × a mix of `edgegrey = 0.75` toward the RGB mean, the fog-of-war grey |
| fade | alpha = clamp(1 + 6 · (0.18 − r²), 0, 1), with r the distance past the edge over the map's size on each axis. Full strength out to **42 %** of the map's size past the edge, gone at **59 %** | linear to black over `edgefade = 1536` world px, measured on the tile grid. `edgesteps = 0` (smooth), `edgedither = 0` |
| relief | `curvature`: the ground drops (d / 150)² elmos with distance d past the edge | none, since TA's terrain is flat art |
| fog | the engine's distance fog | none |
| lit | the map's normals, flipped with the mirror | **unlit**: the art is painted lit from the north-west, and its reflection is lit from wherever the mirror sent that light |

### 1.3 BAR pan and zoom behaviour the lab did not take

These are listed so nothing is lost. Each would be its own decision **[OPEN]**:

* Shift + wheel at ×0.72.
* Alt + wheel, which jumps to the whole map and back.
* A zoom-out floor sized to the map instead of a fixed 0.25×.
* Middle-drag pan.
* The 5 px edge-scroll band. TA's single edge pixel stays.

The lab's left-drag "grab the map" is a lab convenience and was never a BAR rule.

### 1.4 How it goes into the game

`tagpu_zoom.c` owns every piece of this, and its mechanisms stay: one level owner on the render
thread, the incremental eye delta posted to the game thread, the range enforced in engine memory,
and the fog handshake ([GPU status](gpu-status.html), the zoom-to-cursor gates). The work is
five changes. C1–C4 are camera changes (G20a); C5 draws (G20b).

**C1 — the old camera deleted [BUILT, G20a].**

* **No camera setting.** C2–C4 replace the old rules in place. Nothing chooses between cameras,
  so the only camera switch that remains is whether the zoom pass is armed at all.
* **Deleted, callers ported:**
  * the window clamp (`d = (W/2)(1 − 1/z)`, `g_eyeWide`), and the flag that ran the replacement
    clamp only above 1×;
  * zoom-out about the pointer;
  * the ×1.1 notch and its log ease (`WHEEL_STEP`, `WHEEL_EASE`, `eye_level()`, `pred_level()`);
  * `tagpu_zoomedge.off`, the command record's `eyeoff` and its lines in the `ta-drive` skill
    (`SKILL.md`, `references/levers.md`, which now lists it under "Names that do nothing");
  * the lab's `cam=game`, and its `clamp=`, `zoomout=` and `ease=` knobs. The lab's camera is the
    game's camera, and the lab keeps no second one;
  * `tacli wheel`'s help, which now says ×1.163 in and ×0.877 out.

**C2 — the centre clamp [BUILT, G20a].**

* **The range.** `camera_range()` is the one function: lo = −W/2 and hi = map − W/2, with W the
  **true** viewport, or the engine's own [0, extent − W]. `zoom_eye_range` (the game thread's
  inputs) and `range_pk` (the packet's `vp`, `map_pxw/h`, `map_w16/h16` and the new
  `cam_centre`) both call it, which is the invariant the pre-clamp rests on.
* **Two sizes.** `map` is the map's own size, the PLOT grid `main+0x14233`/`+0x14237` × 16 —
  the tile map the lab and the terrain pass draw, and where the mirror (C5) reflects. `extent` is
  the scroll extent `main+0x1422B`/`+0x1422F`, the map less 32 px wide and 128 px tall, which the
  engine's clamp `0x41C3C0` and its inline target clamps read. The view centre stops at the map's
  edge, as the lab's does (Two Continents: `(10304, 12448)` at 1024x768, not the extent's
  `(10272, 12320)`). The engine's range and the pointer's guards stay on the extent, the bound they are
  built on: the 128-px bottom margin is `GetTPosition`'s search window (engine map,
  "Engine defects we patch").
* **Which range is in force.** The centre range holds only on a draw whose ground is ours
  (`s_gCentre = installed && terr`), because the engine's terrain pass `0x483FA0` has no bound on
  its tile index; the terrain latch is set from the same request right after the apply, so the
  two agree on every draw. That bounds the eye the pass reads from, not its window: a view larger
  than the map runs the window past the tile map from any eye, in stock and on both cameras, and
  the window check at `0x484057` bounds it (engine map, "Engine defects we patch"). The apply clamps the eye and target into the range in force on every
  in-play draw; the level end puts the engine's range back and walks the eye into it.
* **The flag.** The replacement of `0x41C3C0` runs whenever it is installed.
* **The three reachable inline target clamps** — not two. The smooth arms of centre-on-point
  `0x41C7C0` and of centre-on-object `0x41C8E0` each store the target and clamp it inline (blocks
  `0x41C808`, `0x41C93B`); the follow's block `0x41CAF7` is the third. All three are replaced, not
  chased: the first instruction jumps to a stub that clamps into the range in force and resumes at
  the function's tail. `0x41C7F7`, which this note named before, is the target store inside
  `0x41C7C0`, not a function of its own. `SetCamera 0x41C4C0` has a smooth arm of the same shape
  (`0x41C4EC`) and it is left alone: all four callers push `smooth = 0` and the function's address
  occurs nowhere in the image as data, so no path reaches it.
* **The debug overlay.** `0x418310` has no lower bound on its cell window, so its one call site
  `0x468DBA` is redirected to call it only for an eye in the engine's own range.
* **The minimap box** is scaled about its centre, then its centre is clamped into the minimap
  and each edge on both sides, so it cannot invert when the view centre is past the extent the
  engine places it by (gpu-status §2.3c).
* **The fog.** An eye off [0, extent − W] puts the engine grid's border completions off the map, so
  the native pass draws from `fogwide`'s grid then (`tagpu_zoom_wide_fog`), and the eye each frame
  is drawn from is bounded into the grid it samples (gpu-status §2.3, "The fog bound on the drawn
  eye").

**The audit C2 needed.** Every engine reader of the eye had to be bounded for an eye W/2 past the
map, **at every zoom**, with `vpwide`'s widened rect below 1× adding W/(2z) − W/2 more. Before
G20a the eye reached −0.4375 W at 8× and never went below 0 at or under 1×. The full table, with
the instruction behind each bound, is [exe reverse engineering](exe-reverse-engineering.html)
§"Who reads the eye". Moving the far ends from the extent's edge to the map's (32 px right,
128 px down) changed no verdict: no row's bound reads where the centre range ends. The rows this
note listed:

| reader | bound |
|---|---|
| `0x483FA0`, the engine's terrain pass | **none of its own** — the tile index from `eye >> 5` is unchecked at both ends. Its eye stays in `[0, extent − W]`: the centre range is in force only on draws whose terrain latch skips it. Its window does not: over a view larger than the map it runs past the tile map from any eye (Lava Run at 1920x1440, 12 skirmish maps at 3840x2160), in stock and on both cameras — bounded by the window check at `0x484057` |
| `0x418310`, the map debug overlay | **none below** — the cell window starts at eye/16 with only the upper ends clipped. Bounded by the redirect of `0x468DBA` |
| `0x498DA0`, the pointer → world point | **bounded twice**: `tagpu_vpwide.c` clamps the world point to the scroll extent, and `zoom_tpos_guard` (`0x498EF9`) clamps the side-panel path to the same. A right-click past the edge orders a move to the nearest point of the extent |
| the minimap view box | **bounded**: its centre clamped into the minimap, then each edge on both sides, at every zoom — it cannot invert |
| `0x4843C0`, the screen fog grid builder | **bounded**: every cell is tested unsigned against the LOS block's w/h before either read; the border completions stay inside the grid, but their content is misplaced for an eye off [0, extent − W], which is why the native pass takes `fogwide`'s grid then |
| `0x4848E0`, the fog draw | **bounded**: a `[0, cols) × [0, rows)` walk; terrown skips it with the terrain pass |
| `fogwide`'s wide grid | **bounded in the replica**: every read of the mapped bytes checks `idx < mappedCells`, and the edge completions derive their row from `row0` |
| our feature gather (`tagpu_feat.c`) | **bounded**: rows and columns are clamped to the map. The mirror's gather (C5) is a separate list, so this clamp stays |
| the engine's sweep rect and HotUnits (`DrawGameScreen`'s row sweep, `0x48BAE0`) | **bounded**: the sweep's start is clamped to 0 and its end to `PLOT − 1`, signed; the bucket index is refused below 0 and at the row count; the cull is a signed rect test. The append past one row's capacity is stock and harmless; past the end of the whole buffer (`0x469807..0x469825`, below 1×) it is a stock defect, patched (engine map, "Engine defects we patch") |
| edge scroll and the scroll target | through `0x41C3C0` and the three reachable target clamps, all ours |
| positional sound, the drag box, the unit hit test, the screenshot tiler, the effect and particle draws | **bounded** or projection only |

Each row ends with a bound argument (*Fixes must be safe by construction*), not with "it did
not crash".

**C3 — zoom out from the centre [BUILT, G20a].** A notch out adds no displacement of its own:
only a notch in contributes a term to `R` (below). Zoom-out never anchors to the pointer.

**C4 — BAR's notch and tween [BUILT, G20a].** They replace `wheel_level()`'s ×1.1 notch and log
ease (`WHEEL_STEP`, `WHEEL_EASE`, `WHEEL_SNAP`'s ease role):

* **The notch.** Each notch multiplies the target *distance* iz = 1/z by `max(0.1, 1 − 0.14 n)`,
  with n = Δ / `WHEEL_DELTA`. That is `ScrollWheelSpeed −20 × 0.007`. The target is clamped to
  [1/8, 4].
* **The tween.** It runs from the level currently drawn, over 250 ms of the render thread's present
  clock (QPC), with g = 1 − (1 − f)⁴ applied to iz, not to log z.
* **What stays.** The 0.999..1.001 snap stays. It costs nothing, and the parity fixtures set the
  level through `tagpu_zoom.txt`, not the wheel, so BAR's off-grid levels never reach a fixture.

**The anchor is the lab's model, kept incrementally.** The lab keeps a target pose (c₁, iz₁):
a notch in at `a` moves it by `c₁ += (a − c)(iz_prev − iz_new)` on the target distances, a notch
out leaves c₁, and the drawn pose is lerped from where it is to the target along the same `g` for
both. For a single notch that line happens to equal holding the pointer's point on every frame;
for a notch that cuts a tween it does not — holding the latest point would re-aim the cut
tween's remaining displacement, and a notch whose drawn level turns downward would drop it. So
the game keeps `R`, the world displacement the tween still owes, and each notch carries its own
point through the ring:

    at a notch   R = R·(1 − g_posted) + (in ? (a − c)(iz_prev − iz_new) : 0);  g_posted = 0
    each frame   post R·(g − g_posted) through the eye delta;                   g_posted = g

`R·(1 − g_posted)` is what the cut tween had not yet posted — the lab's `c₁ − c_drawn` — so the
posts telescope to the sum of the notches' own terms and the final eye matches the lab's final
centre, away from the clamp. The one difference is the clamp: the lab clamps the target centre
at the notch, the game pre-clamps each frame's step per axis, and an axis the clamp cuts drops
the rest of `R` and the residual on it rather than banking them.

**C5 — the mirror, in the Vulkan world pass (G20b).**

* **`edge = mirror | black`, default `mirror` [DECIDED 2026-09-23].** This is an on/off setting,
  the only new one. It is a store key in `impure.cfg` ([renderers](renderers.html) §2.10b) with a
  row in the in-game settings and a lever file for A/Bs, following §2.10b's precedence: a lever
  beats the store, which beats the compiled default. `tagpu_defaults.off` skips the store and the
  play defaults, so a control launch draws `black`. Past the map there is always something to
  draw once the view centre can reach the edge: up to W/2 at 1×, and up to 2 W at 0.25×.
* **Terrain.** Off-map cells join the terrain instances. Each one carries the tile of the cell it
  reflects to, found with the lab's triangle wave `reflect(m, n)`, and a flip bit per axis. It is
  drawn with the lab's tone: `edgeT` from the distance to the map on the tile grid, the grey mix,
  the dim, and the linear fade. This is `EDGE_TONE` in the lab, which ports as GLSL → SPIR-V like
  every lab shader.
* **Features.** They are gathered past the map from the **map's own** anchors (the TNT's, not a
  scenario's):
  * They are flipped across a side edge and stand upright across the top and bottom.
  * A y-mirrored anchor **adds** the half height rather than subtracting it, because the art has
    the height baked in.
  * Their depth keys come from the same base and formula as the map's own, so the two sort
    against each other.
  * They are discarded inside the map rectangle, or the painter's order lets a mirrored tree cover
    the last rows.
* **Units stay on the map.** The minimap is unchanged, as BAR's is.

The game's terrain is instanced: `tagpu_terr.c`'s vertex stage reads one quad plus a per-instance
`aCell` (col, row, cx, cy) as four signed shorts. A mirrored cell is one more instance, with its
tile taken from the reflected cell and its flip bits in spare bits of the instance.

### 1.5 How each step is verified

* **The picture is untouched by C1–C4. Only where the camera goes changes.** At an eye and level
  pinned by `tagpu_eye.txt` and `tagpu_zoom.txt`, every world A/B is **0 px** from the build
  before, for eyes inside the old range. A fixture whose eye the old clamp used to pull in now
  renders where it asked; those fixtures are listed and re-baselined. With `edge=black`, C5 is
  0 px as well.
* **C2.** Scroll to the north-west corner at 1×. The roster's eye reads (−W/2, −H/2), and a shot
  has the map's corner at the view centre. Repeat at 0.25× and 8×, and repeat on the south-east
  corner. Run a scripted edge scroll for a minute at each zoom, and wheel at every edge with
  `tagpu.log` clean.
* **C3.** Wheel out with the pointer off-centre. The world point at the view centre
  (eye + W/2) is unchanged across the notches, give or take the rounding the residual carries.
* **C4.** The log line reads `zoom: wheel +120 -> 1.163` for one notch in from 1×. The level
  reaches its target between 240 and 260 ms after the notch, read from the per-frame level. The
  zoom-to-cursor gate still holds: the world point under the pointer moves < 1 px across a notch
  in.
* **C5.** In a shot at 0.5× on a map corner, the off-map strip equals the flipped on-map strip
  through the tone. Recompute the tone offline from the shot's own on-map pixels and compare to
  within rounding. Then run an A/B against the lab at the same eye and zoom, using the pack the
  game shot was taken from.

---

## Part 2 — Classic on the full-colour pipeline

**The scope [DECIDED 2026-09-23].** Classic moves onto the Classic++ pipeline with its rendering
options off, and the 8bpp path is deleted from the world passes.

- **Full colour is the goal, not fidelity to the 8bpp colours.** Shading and the fog grey are
  computed in RGB and never snapped back to the palette. So Classic's old "must not move by a
  pixel" claim is re-baselined once (§2.4, 2b).
- **The UI stays indexed.** Its source is the engine's own 8-bit drawing into its surfaces, and it
  already has a colour twin for Classic++.
- **The GDI fallback is untouched.** It never used this path.

### 2.1 What the code is today  [SOURCE, surveyed 2026-09-23]

**The output is already RGB end to end.** Every world fragment shader writes RGB into the
`ss×` world target, and `native_d` composites that onto the swapchain. "8bpp" survives in two
places only:

- **R8 index atlases:** terrain, features, effects and units, sampled `NEAREST`.
- **index→index arithmetic:**
  - `PALETTE.SHD`'s face-shade row for units (`tagpu_native.c`'s unit FS, `uLUT`, under `uLit == 0`);
  - the engine's fog grey table (`TAGPU_GLSL_FOG_SHADE`, `uFogLUT`, copied by `tagpu_packet_pub.c`'s `fogshade_snapshot`);
  - markers resolved through the same fog LUT.

| pass | Classic samples | Classic++ samples | the switch |
|---|---|---|---|
| terrain (`tagpu_terr.c` FS, `tagpu_vk_terr.c`) | R8 → fog LUT → `uPal` | the RGBA twin where its alpha > 0.5, else `uPal[index]`; × lambert; RGB-mean grey | `uLit` = `tagpu_classicpp_on()`, `uLambert` = `_lit()`, `uRestored` = `_assets()` and the twin painted |
| features (`tagpu_feat.c`, `tagpu_vk_feat.c`) | R8 → fog LUT → `uPal` × alpha | twin or `uPal`; × the ground lambert at the anchor (`vLam`); RGB-mean grey | the same three |
| units (`tagpu_posedraw.c` VS + `tagpu_native.c` FS, `tagpu_vk_unit.c`) | R8 → **SHD row** → fog LUT → `uPal` | twin or `uPal`; × `taLambert` of the **flat up normal**; RGB-mean grey | the same three |
| effects (`tagpu_fx.c`, `tagpu_vk_fx.c`) | R8 → `uPal` | twin or `uPal` | `uRestored` only. The grey band discards effects |
| markers (`tagpu_mark.c`) | index → fog LUT → `uPal` | the same (no Classic++ branch) | — |

Four facts the plan rests on:

- **There is no stored "raw" RGBA atlas.** Classic++'s fallback for a texel the restorer has not
  painted is computed per fragment, `t.a > 0.5 ? t.rgb : uPal[index]`. So Classic++ still reads
  every R8 atlas.
- **Units are never shaded under Classic++.** `tagpu_posedraw.c` publishes `lambert = 0`
  ("never set"), so a Classic++ unit is lit by the flat up normal. That is exactly 1.0, with no
  face shading at any `light=` value. The SHD row is only ever applied in Classic.
- **Gamma.**
  - The presented palette (`tagpu_pal_live()`) is `min(255, entry × factor)`, with factor
    `0.5 + Gamma/24` (`SetGamma 0x4BA590`), so the stock Gamma of 12 is exactly 1.0.
  - Classic is exact under Gamma only because it looks the palette up last.
  - The restorer's input is that **presented** palette (`s_pub.pal = tagpu_pal_live()`), so the
    Classic++ twins bake the Gamma in and go stale when it moves.
  - Only the terrain (`restore_publish(…, repaint)`) and the UI repaint on a palette change. The
    feature, effect and unit twins have no trigger.
  - [GPU status](gpu-status.html)' paragraph on `tagpu_rglsl_job_repalette` describes a mechanism
    that no longer exists.
- **Team colour is a texture choice** (`frame[owner]`, `tagpu_render3do.c`), not a palette remap,
  so it survives a full-colour atlas unchanged. The same holds for **translucency**: features'
  `MODE_ALPHA`, cloak and the build ghost are already RGB blends.

**Measured beforehand.**

- **In the lab**, the full-colour path with every extra off differs from Classic on **0.033 % of
  pixels at 1×** (603 px): 0 px of terrain, 7 px of features, and 416 px of units. The unit pixels
  are all the SHD face shade ([tascene](tascene-design.html)).
- **In the game (G18a, GL lane)**, `assets=0 light=0` reproduced Classic to within 594 px on one
  unit, falling to 415 px once the hard shadow was on (`shadows=2`). The residual was the SHD row
  again.

### 2.2 The target

One pipeline, two presets. The Renderer row keeps Classic / Classic++ / Custom, and the
"Undithered assets" and "Dynamic lighting" rows keep their meaning. `assets=0` now means the
**raw full-colour** atlas instead of 8bpp indices.

| | Classic (`assets=0 light=0`) | Classic++ (`assets=1 light=1`) |
|---|---|---|
| texels | the **base atlas**: each index expanded to its palette colour, RGBA8, alpha 0 at the frame's key | the restored twin over the same base |
| terrain | unlit | lambert from the heightfield, level ground exactly 1.0 |
| features | unlit | the ground's lambert at the anchor |
| units | the **face-shade multiplier** (below) | the **same multiplier** [DECIDED 2026-09-23], where today they are flat |
| fog of war | the RGB mean (`TAGPU_GLSL_FOG_GREY_RGB`) | the same |
| shadows | as the Shadows row says (hard by default) | the same |
| sampling | `NEAREST` | as today (units trilinear, 4× anisotropic) |
| Gamma | **once, on the finished world image** [DECIDED 2026-09-23] | the same |

**The face-shade multiplier [DECIDED 2026-09-23: an RGB multiplier].** The vertex stage keeps
choosing a shade row exactly as it does now:
`row = clamp(neutral + dir · round(12 · N·SH_L), 0, 31)` (`tagpu_posedraw.c`). The fragment then
multiplies RGB by `k[row]` instead of remapping the index through the row, and clamps to 1.

`k` is 32 floats, built once from the engine's own SHD table, which the packet already carries
(`tagpu_r3d_lut_want`). Each row's value is the least-squares slope of shaded against unshaded
palette colour, ignoring channels the table clips at 255, and normalised so the neutral row is
exactly 1.0.

**MEASURED on the stock `PALETTE.SHD`** (through the lab's pack):

- The neutral row is **15**. The rows a unit can reach are 3..27.
- `k` runs from **0.196** at row 3, through 0.542 at row 8, 1.000 at row 15 and 1.391 at row 22,
  to **1.603** at row 27. The table is close to linear below neutral: about 0.066 per row.
- How well a pure multiply fits the table, as mean |error| in levels:
  - row 4: 4.7
  - row 16: 5.6
  - row 22: 9.3
  - row 28: 14.6

  The bright rows brighten less like a multiply than the dark ones darken, because they clip and
  desaturate. That is the look to check at 2b. It is a fact about the table, not a decision.

### 2.3 What stays, what goes

**Stays:**

- the palette as a **256-colour table** for everything that is a colour index and not an atlas
  texel: untextured 3DO faces (`vFC`), the nanoframe band ramp (0xA0..0xAF), markers and their
  selection colours, effect lines (mode 0), and unexplored black (index 0);
- the LHT flash table (`uLht`, already RGB);
- the UI's RG8 twins;
- the restorer. Its FILL stage reads the base atlas instead of doing the palette lookup itself.

**Goes, at 2d:**

- the R8 atlases and their samplers;
- the `uLit == 0` branches in the terrain, feature and unit shaders;
- `uLUT` (the SHD texture) and the per-index row lookup;
- `uFogLUT`, `TAGPU_GLSL_FOG_SHADE`, `fogshade_snapshot` and its field in the packet;
- the per-pass `tagpu_classicpp_on()` gate. Classic vs Classic++ becomes a preset over `assets`
  and `light` and nothing else.

`tagpu_classicpp.on/.off` stay as the levers that pick a preset, which is what tacli drives. No
alias is kept for anything deleted (*strip fully*).

### 2.4 The steps

**2a — the base atlas.** Every world atlas gains an RGBA8 base beside its R8, filled at upload
on the CPU from the index data and the palette. Alpha is 0 where the index is the frame's own key.
Classic++'s fallback becomes `twin.a > 0.5 ? twin.rgb : base.rgb`. The restorer's FILL reads the
base.

In this step the base is expanded from the **presented** palette and re-expanded on the palette
serial, exactly as the restored twins are, so no picture moves.

- **Exit:** Classic++ is **0 px** from the build before, per pass, on the world A/B
  (`tagpu_<pass>.ab`, `tools/vk-ab.py`), at two Gamma settings. Classic is 0 px too, because it
  still reads the R8.

**2b — Classic onto the full-colour shaders.** The Classic preset takes the Classic++ branch with
`assets=0 light=0`:

- the face-shade multiplier for units, in **both** presets;
- the RGB-mean grey in every world pass, markers included.

The old index branch is still in the build, behind the lever, for one landing. That is what makes
the re-baseline checkable: the old Classic and the new one can both be shot from the same build.
The lab's Classic lane moves to full colour in the same landing, so the lab and the game still A/B.

- **Exit:**
  - old against new Classic, measured and written down, per pass and in the grey band;
  - the new lab md5s and A/B captures recorded as the new baseline;
  - Classic++ differs from the build before on unit pixels only (the multiplier) — measured, and
    shown to the owner;
  - the owner looks at the new Classic beside the old one, at 1× and zoomed, in and out of fog.

**2c — Gamma once, at the end.** Everything that makes a world colour — the base atlases, the
restored twins, `uPal` — is built from the **engine's** unscaled table (`tagpu_pal_engine()`).
The world composite multiplies by `tagpu_pal_gamma()` and clamps. The UI keeps the presented
palette, so the two agree.

This deletes every world repaint-on-palette path, and with it the gap where the feature, effect
and unit twins never recoloured. [GPU status](gpu-status.html)' repaint paragraph is corrected in
the same landing.

**[INFERRED — verify first]:** Gamma is the only thing that moves the palette in play.
[Renderers](renderers.html) §2.3 establishes that water does not cycle it.

- **Exit:**
  - at Gamma 12 (factor 1.0), **0 px** from 2b in both presets;
  - over a sweep of Gamma 0, 6, 12 and 20, the differences from 2b lie only where a shaded or
    blended colour clips — counted and reported;
  - after a Gamma change in play, every pass shows the new factor at once, with no repaint queued.

**2d — delete the 8bpp path** (§2.3's list). SPIR-V is regenerated (`spirv-gen.py` hashes its own
comments, so regenerate rather than hand-edit).

- **Exit:**
  - both presets **0 px** from 2c on every pass;
  - `thread-split-check.sh` clean with the packet's fog-table field gone;
  - atlas memory reported before and after. The R8 goes and the base stays, so the total is
    3 B/texel over today's R8 — about 12 MB per 2048² atlas;
  - the function inventory diffed against HEAD, because a deleted non-static compiles clean.

---

## Landings and reviews

**Two tracks run in parallel, and the mirror waits for the second.**

- **Track A** is G20a, the camera (C1–C4): `tagpu_zoom.c`, the three target-clamp blocks and the
  debug-overlay guard, and the lab's camera.
- **Track B** is G20c then G20d, full colour (Part 2): the atlases, the world shaders, the
  restorer's input, the packet and the composite.
- **The overlap** is `tagpu_settings.c` and `tagpu_menu.c`: G20b adds the `edge` key and row, and
  B changes what `assets=0` means. Both are small additions, so they merge cleanly.
- **The mirror (G20b) builds on Track B's base atlas.** It edits the same terrain and feature
  shaders and passes that G20c rewrites. Built after G20c it has one colour path, RGB in with the
  tone applied; built before, it would need an index path that G20d then deletes.
- **Track A's own 0 px gate** is against its own base. After merging `main` in, compare against
  the merged tree, because G20c changes Classic's pixels on purpose.
- **Take C4's ease timing with Track B's instance stopped**, so that the other game on the same
  GPU does not add noise.

| landing | contents | review (CLAUDE.md) |
|---|---|---|
| 1 (track A) | C1–C4: the BAR camera in, the old rules out | **high** — writes engine memory (the eye, the target and their range) and adds byte patches at `0x41C808`, `0x41C93B`, `0x41CAF7` and `0x468DBA` |
| 2 (after 3) | C5, the mirror | medium — new instances and a new sprite list in two passes, no engine state |
| 3 (track B) | 2a + 2b | medium — atlases and shaders |
| 4 (track B) | 2c + 2d | **high** — the packet loses a field, which is the game↔render hand-over |

## Decisions [DECIDED 2026-09-23, the owner]

1. **Pan and zoom only.** The projection, tilt, rotation and FOV do not change.
2. **One camera.** BAR's rules replace the game's camera rules outright, and the old rules are
   stripped. There is no camera setting, no lever and no fallback.
3. **`edge` is an on/off setting,** `mirror` by default.
4. **Classic is full colour.** No fidelity to the 8bpp colours is kept.
5. **Unit face shading is an RGB multiplier** from the SHD table.
6. **Classic++ units get the same face shading** as Classic.
7. **Gamma is applied once, on the finished world image.**
8. **The UI stays indexed.**
