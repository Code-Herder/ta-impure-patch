# Levers: every arm file, its tokens, and what its heartbeat must say

Read this when you arm anything beyond the bench line, or when a `tagpu.log` heartbeat has to be
read. Every file here is `gamedir/tagpu_<name>` written by `tacli arm <i> <name>[=tokens]`. The
authority is `tagpu/ddraw/src`; if a lever named here is not read there, delete it here.

1. [How a lever works](#how-a-lever-works)
2. [The play defaults](#the-play-defaults)
3. [The world passes](#the-world-passes)
4. [The marker passes](#the-marker-passes)
5. [Camera, viewport and fog](#camera-viewport-and-fog)
6. [Classic++](#classic)
7. [The Vulkan lane](#the-vulkan-lane)
8. [Plumbing and safety](#plumbing-and-safety)
9. [Instruments](#instruments)
10. [The heartbeats: what must read zero](#the-heartbeats-what-must-read-zero)
11. [Names that do nothing](#names-that-do-nothing)

## How a lever works

- **The file's presence arms it; its text is the token list** (`tacli arm <i> 'fx.on=log
  passive'`). `off`, `0`, `false` or `-` as the value unlinks the file.
- **Three read times.** *Attach*: the code-patching halves (`owndraw`, `terrown`, `featown`,
  `fxown`, `markown`), `zoom`, `vpwide`, `weapons`, `cobtrace`, `reclaim`, `packet`, `hud`,
  `lerp`, `curs.off`, the `gui` hook tokens and `restoreglsl.on` — arm before the launch that
  matters, and relaunch to change them. *Live* (polled, 250–500 ms, or every frame): the draw
  passes' tokens (`native`, `terr`, `feat`, `fx`, `sfx`, `mark`, `order`), `classicpp` and its
  `.cfg`, `vk.on`'s `color=`, the `gui` surf tokens, the `.off` toggles. *One-shot*
  (self-deleting): `posedump.on`, `surfdump.on`, `gui_census.trigger`, the `.ab` files.
- **Precedence: an `.on` beats an `.off`.** An `.off` only turns off a pass that is on *by
  default*; with both files present the `.on` wins, tokens and all.
- **Removal survives a relaunch** (the gamedir refresh skips `tagpu*` files).

## The play defaults

With no `tagpu_defaults.off` in the gamedir the DLL arms these by itself (`tagpu_opt.c`); a
`tacli` instance writes that file unless launched with `--defaults`, so on a tacli instance only
the arm files count. A dependency turns off with its parent.

| default | tokens | depends on |
|---|---|---|
| `native.on` | `all wrecks` | |
| `terr.on`, `feat.on`, `fx.on`, `sfx.on`, `mark.on`, `order.on` | | |
| `markown.on` | | `mark.on` |
| `terrown.on` | | `terr.on` |
| `ghost.on` | | `native.on` |
| `zoom.on` | | |
| `vpwide.on` | | `zoom.on` |
| `gui.on` | | |
| `classicpp.on` | | |
| `weapons.on` | | |

Not on the defaults, and why: `owndraw`, `featown`, `fxown` stop the engine drawing what our pass
draws and so hole the golden source (`tacli` still auto-arms them when the pass is armed, because
they save the engine's CPU); `hud.on` changes the look of every screen and is the owner's call;
`scaffold.on` and `fps.on` are instruments.

**`terrown` is the exception and is a default.** It holes the golden source like the other three
— the viewport arrives key-filled, with no terrain — but it is an **optimisation, not a
requirement**: the play defaults and the play defaults plus `tagpu_terrown.off` render the same
picture (`build-facing` at 1024x768: 0.03 % exactly-black viewport and 8898 distinct colours
either way), and with it off `tacli shot` has terrain again. **`tacli arm <i> terrown.off` is the
arm for a reference-quality capture.** What it costs is the **zoomed-out fog**, not the picture:
`tagpu_fogwide`'s tick runs inside `terrown`'s fog-overlay detour and only while the skip is set,
so a zoomed-out frame falls back to the engine's 1x grid.

## The world passes

Each pass has a draw half (the `.on`, live tokens) and, where the engine's own drawing has to be
stopped, a patch half installed at attach that `launch`/`scenario load` auto-arm (`auto-armed
… must be installed at launch, not after`). Every pass logs one `ARMED` line at launch; a missing
line is the whole pass silently absent. `log` on any of them adds a heartbeat; `passive` is the
same-fight A/B lever: the pass keeps gathering and counting while drawing nothing.

| pass | tokens | patch half | verify at launch |
|---|---|---|---|
| `native.on` | `all` or a unit-type filter (a name matching nothing keeps the call site alive without drawing units), `wrecks` (3D husks; **required** for the feature pass to own its leaf) | `owndraw.on` | the `owndraw:` line |
| `terr.on` | `log passive over key=N` | `terrown.on` | `terr: ARMED (log= passive= over= key=254)` |
| `feat.on` | `log passive noflat notall noshadow nowreck` | `featown.on` | `feat: ARMED (flat= tall= shadow= wreck= log= passive=)` |
| `fx.on` | `log passive nolines nomodels nosprites noexpl nodebris` | `fxown.on` | `fx: ARMED (lines= models= sprites= expl= debris= log= passive=)` |
| `sfx.on` | `log passive nosmoke nofire nowake nonano` | `fxown.on` | `sfx: ARMED (smoke= fire= wake= nano= log= passive=)` |

- **`native.on`** is the unit renderer (`tagpu_posedraw.c` is its posed program; no other
  lever names it). The engine rasterises every unit whatever `owndraw.on` says; its detours skip
  only a husk while `wrecks` is armed, the build-state effect of a unit this pass owns, and the
  cached structure shadow once ours is live. A stale `owndraw.on` with `native.on` cleared is
  dropped by `launch`, which says so. The `native:` heartbeat every 300 frames is `native: vulkan
  lane handed over frame N: terr= feat= fx= mark= units= posed= sel=N/M selcache= full=
  fog=wide|engine|none bare=N out=N held=N/Mpx paused=N back=N` — what was handed over to the Vulkan passes, not what
  they drew; the fog fields are below. A model that will not bake (over 256
  pieces or 49 152 vertices) logs a `posebake: REFUSED` line and draws nothing. Nothing reads
  `gamedir/hires/`: a `.glb` there changes nothing, because replacement meshes are not in this build.
- **`terr.on`** owns the terrain and the fog overlay; `key=N` moves the palette index the engine's
  viewport is filled with (254). Without `terrown.on` it emits nothing and its `terr:` line says why: our terrain is opaque
  over the whole viewport, so drawing it with no key fill to invert against would hide every
  engine overlay and still look right. Arming `terr.on` after launch does nothing visible —
  relaunch. Expect `terr: atlas built
  2176x<h> for <n> tiles`, `terr: height grid WxH uploaded` (without it Classic++ terrain draws
  unlit), and `vk: terr: the Vulkan edition is up …`. Under Classic++ `assets=1` it draws the
  restored atlas.
- **`feat.on`** takes the draw only while `native.on` carries `wrecks` (3D wreckage is drawn from
  the same leaf); otherwise it logs `nothing emitted: native.on needs "wrecks"` and `atlas=0`.
  Heartbeat under `log`: `feat: rect=… anchors= flat= tall= … 3dwreck= body= shadow=` every 60
  frames. Fixture `feat-forest`.
- **`fx.on`** is called from inside the native pass, so it needs `native.on` (any filter). The
  engine skip follows the file live: `fx.on=off` restores the engine's effects within 30 frames.
  Heartbeat `fx: proj= expl= … lines= sprites= flashq= models=`. Fixtures `fx-mix`, `fx-lasers`
  (burns out in tens of seconds), `fx-rockets` (stays alive).
- **`sfx.on`** — the ten particle layers; run it with `native.on=all` or the low layers (wake
  foam) composite over engine-drawn hulls. Heartbeat `sfx: layers L2=44(wake:44) L6=26(nano:26)
  …`. Fixture `sfx-strait` (damaged structures smoke, boats wake, a nanoframe). A scripted
  `repair` does not make a builder nanolathe: select it, `ui click ARMORDERS`, `ui click
  ARMREPAIR`, then `keys mouse:X,Y` and `click X Y` on the frame; the spray stops when metal hits
  zero; `ctrl+d` on a selected structure gives burning debris.

## The marker passes

| pass | tokens | patch half | verify |
|---|---|---|---|
| `mark.on` | `log passive nobars nodigits noselbox nocursor` | `markown.on` | `mark: ARMED (log= passive= bars= …)` |
| `order.on` | `log passive trace nobuild nodots nocircle nosprite noranges nolabels` | `markown.on` | `order: ARMED (log= passive= trace= build= …)` |

`mark.on` draws health bars, group digits, the build-cursor footprint, the drag band box and the
text (the group digit, the `ShowRanges` labels), all re-drawn from engine state at native
resolution; `order.on` walks the order lists on the game thread and draws the waypoint, build-site
and patrol markers with their range circles and labels. `markown` is a **producer** as well as a
suppressor: its redirects fill the order arena and the packet's font copy, so with it absent no
marker draws and no text draws anywhere (the packet line reads `font=0/0B`). `mark.on=passive`
hands every marker back to the engine's own drawing (into the golden source) while ours count.

**AND IT IS THE GOLDEN SOURCE ONLY — THE ENGINE'S COPY DOES NOT REACH THE SCREEN.** The world
phase stamps everything the engine draws between `0x468DB0`–`0x468E3A` and `0x469849`–`0x469F36`
and `publish` refuses it, so **every configuration that leaves a world draw to the engine shows it
in `tacli shot` and not on the window**: `mark.on=passive|nobars|noselbox|nocursor|nodigits`,
`fx.on=passive`, `sfx.on=passive`, and equally any pass left unarmed while its `*own` half is also
absent — no `native.on` without `owndraw.on` loses the **units** (`DrawUnit 0x45AC20` at
`0x469A00`/`0x469BA3`), no `terr.on` without `terrown.on` loses the terrain, no `feat.on` without
`featown.on` loses the trees. At the play defaults our own passes draw all of it and nothing is
lost, and it is all still in the shot — which is what `passive` is *for*. But as a way of
**forcing engine pixels onto the screen** every one of those is blunted: with `noselbox` and a
unit selected the box is in the golden source and absent from the window. The lever for that job
is **`worldphase.off`**, which turns the phase off wholesale and restores all of them at once.
(The marker row is the measured one; the others follow from the same mechanism and the call sites
— gpu-status.md §2.81 has the table.)

**`mark.on` arms the pass and opens none of its buckets.** Each draw kind has its own gate, and a
fixture that only arms the pass produces no picture:

| kind | what makes it non-empty |
|---|---|
| health bars | a unit of the **watched** player on screen, and the `damagebars` registry value set (`main+0x37F06` bit 0, read from the packet's copy) |
| selection rects | a unit of the local player **selected** (`ctrl+a`, or a `click` on it) and the engine's `SelBoxes` toggle on (the default); not under `noselbox` or `passive` |
| group digits | `u->squad` non-zero — `tacli keys <i> ctrl+1` on a selection; `damagebars` too |
| order lines | `order.on` **and** SHIFT physically held (`down:shift` … `up:shift`; a bare `shift` is a 150 ms tap) **and** an order that does not complete — `move` completes and takes its markers with it; `patrol` never does |
| order triangles | the route dots — only for the **hovered** unit; `pmove:x,y` onto it from a fresh roster, at game speed 1 so it does not walk away |
| labels | `+showranges`, typed into the chat |
| cursors | a **held** drag (`pmove:x0,y0`, `down:lbutton`, `pmove:x1,y1`, capture, `up:lbutton`) or a build placement |
| post-fog layer | `mark.on=nocursor`, the only window in normal play that still fills it |

- `order: arena=-1` in the heartbeat is normal: SHIFT is not held.
- `order.on=trace` runs both sides in one pass and logs both node lists (`order TRACE own:` /
  `order TRACE eng:`), which is the correctness gate — a pixel diff is unavailable because native
  resolution means the frames deliberately differ.
- `mark.on=noselbox` stops our selection rects and hands the box to the engine, which draws it at
  the unzoomed projection into `tacli shot` only (`tacli log <i> -g 'markown: engine selection'`
  → `restored`). With `worldphase.off` as well, the engine's boxes reach the window.
- The selection-rect count is in the native heartbeat: `native: vulkan lane handed over … sel=N/M
  selcache=K full=F` — `N` rects handed to the marker pass of `M` selected units on screen;
  `mark.on=log` adds `sel=` and `selover=` (rects past the bucket) to the `mark:` line.
- `ShowRanges` is a typed cheat, not a switch: `keys <i> return`, then `char:+ char:s char:h …`,
  then `keys <i> return`, and confirm with `tacli peek <i> '*0x511DE8+0x391BF:4'` — on a
  `scenario load` fixture the chat line may not open under injection and the address stays 0.

## Camera, viewport and fog

| lever | when read | what |
|---|---|---|
| `zoom.on` | attach | the wheel, `tagpu_zoom.txt`, the minimap view rectangle, the guard that keeps our `ScrollSpeed` scaling out of the registry, and the camera's centre range. Logs `zoom: ARMED (minimap rect 0x466B70 x2, ScrollSpeed save 0x430FAE, camera centre range 0x41C3C0 + target clamps …)`. It also installs the `0x498DA0` mouse->world repair, which is what the wheel and the file need; `vpwide.on` installs that repair too, so either one un-pins the level. With neither: `zoom: PINNED AT 1.0 — the 0x498DA0 mouse->world repair is not installed` |
| `tagpu_zoom.txt` | every frame | a bare float 0.25–8.0; written atomically (temp + rename); pins the level and disables the wheel while present; does not move the camera |
| `tagpu_eye.txt` | every in-play draw | what `tacli eye X Y` writes; the packet heartbeat's `hold=1` says it is in force; `tacli eye <i> --release` removes it |
| `wheel.off` | live | the wheel does nothing; `wheel.off=off` removes it. Nothing arms the wheel separately: it comes with the mouse->world repair (`zoom.on` or `vpwide.on`) |
| `vpwide.on` | attach | widens the rect the engine addresses to what the zoom shows, so ring clicks and band boxes land at zoom < 1. Writes `main+0x37E27..0x37E33`. Logs `vpwide: ARMED (mouse->world 0x498DA0, surface …)` and `vpwide: true viewport rect verified (128,32 896x704)`, `vpwide: viewport rect restored to 1x` at 1x. `zoom.on` alone logs `vpwide: mouse->world repair only (0x498DA0) —` |
| `fogwide.off` | live | the wide fog grid off: the outer ring at zoom < 1 falls back to a smear of the border cell, and the native line's `bare=` and `out=` count every such frame |
| `fogwide_check.on` | live | the oracle: `fogwide check: … compared=N of cells=M skipped=K differ=N` every 120th tick, **`differ=0`**. It compares only the entries both builders define the same way: `skipped` is the border lines where the engine's literal completion row or column and ours (the straddling one) differ, both left out — 0 for an eye in the engine's own `[0, extent − W]`, non-zero near the map's edge in the centre range (104 of 720 at a corner, 1024x768) |

Driving the camera at a zoom other than 1:

- **The camera keeps the view centre on the map, at every zoom**: the eye ranges over
  `[−W/2, map − W/2]` (W, H the true viewport, 896x704 at 1024x768; `map` the PLOT grid × 16),
  so a map edge can reach the middle of the screen. That range holds only while `terr.on` draws
  the ground; without it the eye stays in the engine's own `[0, extent − W]`, `extent` the scroll
  extent `main+0x1422B`/`+0x1422F` — the map less 32 px wide and 128 px tall, and **not** the
  map's size (Two Continents: map 10752x12800, extent 10720x12672). `tacli eye` is clamped into
  the range in force, so `tacli eye <i> -99999 -99999` puts the map's NW corner at the view
  centre and `99999 99999` its SE corner. A right-click past the edge orders a move to the nearest
  point of the extent.
- **A wheel round trip does not land on 1x** (x1.163 then x0.877 is 1.020). Re-anchor by writing
  `1.0` to `tagpu_zoom.txt`, then deleting it. Deleting the file hands over, it does not reset;
  notches sent while the file is present are dropped and logged (`zoom: wheel … ignored -
  tagpu_zoom.txt is in force`).
- **Jump the camera with the minimap, not the arrow keys.** A *held* left button on the minimap
  lands the eye exactly on `world − (W/2, H/2)` before the clamp:
  `keys <i> mouse:10,0 down:lbutton`, peek `+0x1431F`/`+0x14323`, `keys <i> up:lbutton`.
  `pclick:` alone does not work here; the jump wants the button held across a frame.
- **Put the camera where you want it by scrolling when the fog matters.** The engine rebuilds
  its fog grid only when a camera *move* clears its is-current bit; `tacli eye` writes the eye and
  the scroll target together, so nothing clears it and the stale grid is drawn at the new
  position — a lit LOS circle over a base you never scouted, which looks like a fog bug and is
  not. `keys <i> mouse:0,1079` and wait.
- **Dialogs drawn inside the viewport keep 1:1 clicks at every zoom** (`ARMOPT`, `EXITMENU`,
  `YESORNO`, the preferences screens); `SHARE.GUI` is the known gap, and `ui press <gadget>` is
  the fallback there.
- **Edge scroll is an equality on the outermost pixel** (`x == 0`, `y == 0`, `x == W−1`,
  `y == H−1`), at every zoom on all four edges.

The wide fog grid (`tagpu_fogwide.c`) builds at every zoom from the screen size; its heartbeat is
`fogwide: CxR cells=N cap=CxR rebuilds=N in 5.0s = R/s ticks=N build=…/… us (mean/max)` once
per five seconds of wall time. A video-mode change grows the set, freeing the old block on the
spot (nothing but the game thread reads it), and logs `fogwide: grid CxR, N KB (N cells) — grown`.
**`rebuilds=0` is not a fault at LosType 12**
(`--los 0`): nothing stamps, so nothing rebuilds — read the word at `*0x511DE8+0x14281` before
chasing it; at 14 a moving scene gives ~30/s.

The native heartbeat's fog fields are the fog bound's witness (`tagpu_zoom.c`, gpu-status §2.3):
`fog=` is the grid the last frame actually sampled (`engine` on a bare frame, `none` with no
grid at all); **`out=` must read 0** — frames whose fog domain was not inside that grid; `bare=`
counts frames that needed the wide grid and had none; `held=N/Mpx` is the frames whose drawn eye
the bound held back and the largest hold, never more than the displacement the gesture has posted
and the game thread not yet applied, less the lead the wide grid carries (a quarter of the view a
side) — near 0 when the game thread keeps up (4 frames over 36 wheel gestures at 1080p on the
reference setup's GPU), more when it lags;
`paused=` is the frames drawn at the previous frame's level so the view would not move against
the gesture; **`back=` must read 0** — frames that moved against it anyway.
**A level that starts below 1× shows `bare=` and `out=` of about 15** — the frames before the
terrain pass owns the ground, when there is no wide grid to take — and they must not grow after.

## Classic++

| lever | when read | what |
|---|---|---|
| `classicpp.on` | polled twice a second | the master arm: restored true-colour terrain, features, effects, unit textures **and the UI** (sidebar, minimap, top bar, shell), the lambert lighting and the cast shadows. Flips live; arming mid-play restores what is on screen |
| `classicpp.cfg` | live (re-read on the poll when its mtime or size changes) | the knobs: `sun=AZ,EL` or `sun=off`, `unitsun=AZ,EL`, `amb=A`, `assets=0|1`, `light=0|1`, `shadows=0|1|2`, `shadowsun=AZ,EL`, `penumbra=K`, `shadowlen=A,B` or `off`, `shade=S`, `terrainshadow=0|1`, `shadowres=N`, `airshadow=len|physical|drop`, `aniso=N`. Written by `tacli arm <i> 'classicpp.cfg=sun=off shadows=0'`, removed by `classicpp.cfg=off` |
| `restoreglsl.on` | when the restorer starts (arm before launch) | the restorer core's knobs, in the file that keeps the name it had: `log` (a line per batch), `tiny` (the small model), `fp16`, and the numeric knobs in `tagpu_restore_core.c` |
| `restoredump.on` | after each queue drains | writes `tagpu_restore_<tag>_vk.{r8,rgba,idx}` per atlas — the byte oracle (`references/measuring.md`) |

- The DLL answers every cfg read on its own lines: `classicpp: assets=1 light=1 (…)`,
  `classicpp: light sun=… unitsun=… amb=… level=…/…`, `classicpp: shadows=2(hard) shadowsun=…`;
  `tacli log <i> -g 'classicpp:' | tail -3` says what the frame is lit by. Allow ~1.5 s after
  arming before a shot.
- `assets=` is the restored atlases and is what arms the restorer — there is no second lever;
  `light=` the lambert; `shadows=` 0 none, 2 Classic's own hard silhouette and structure slant
  (**the default**), 1 the soft map-anchored depth map, which has no producer on this lane and
  draws nothing, so the render-options row offers only `Off|Hard` and `shadowres=`/`penumbra=`/
  `shadowlen=`/`shadowsun=` are read by nothing. Shadows also need the engine's own shadow bits
  2, 3 **and** 4 of the option word, and the menu's Shadows row (`SHADOWS`, Options → Visuals
  or the cog) is what sets them now — from the store under `--defaults`, directly without it,
  where it is the one live row. `main+0x37F06` reads `0x3E` on and `0x22` off; peek it before a
  shot: the registry holding the word is one `user.reg` every instance shares, so a shadows-off
  left by another run shows up in a control launch.
- `sun=off` is exactly `light=0`. `light=0` leaves a unit **unshaded**, not Classic-shaded (the
  engine's per-face shade row is the Classic branch's). `assets=0 light=0 shadows=0` is the one
  Classic++ state that is a Classic frame.
- `terrainshadow=1` self-shadows the ground (default 0); at the default sun elevation it is
  nearly invisible, so the fixture for the map is `shadowsun=225,8` on `static-terrain`.
- `aniso=1` for a unit-pass A/B; 4 ships.
- Read the result of a restore in the `restorevk:` lines — `restorevk: <tag>: lazy restore armed
  (…)`, `restorevk: <tag>: done: N frames (… wrap-padded) in B batches, D draws in S of F frames = …`
  and, with `restoredump.on`, `restorevk: <tag>: restored atlas dumped to …`. The tags are
  `terr`, `feat`, `fx`, `unit` and `gui`. The fps in the
  `done` line is the rate the game held during the restore. The first job of every launch is abandoned by the
  startup reset and restarted; the `done` line is the second job's. Wait for it before a parity
  capture.

## The Vulkan lane

The Vulkan backend is the renderer: `ddraw.ini` says `renderer=vulkan` (tacli writes it; the
`openglcore` spelling reaches the same lane through a fallback that logs), and `renderer=gdi` is
the other lane, on which nothing of ours draws, no golden source is captured, and every `tacli`
verb except `eye`, `wheel` and `gui` still works.

| lever | what |
|---|---|
| `vk.on` | not needed to run the lane; read for `color=r,g,b`, the clear colour (black by default; `255,0,255` makes every undrawn pixel a magenta sentinel) |
| `vk.off` | **ignored** under `renderer=vulkan` (the log says so) |
| `tagpu_vk.gpus` | the GPU row's device list (one launch behind); the choice is `gpu=` in `impure.cfg` — `references/modules.md` |
| `ss.off` | the 2x supersample off, and the lever over the Supersampling row. Live: the world target (`gw*ss x gh*ss`) is rebuilt on the next frames — `vk: world: frame N: 1024x768 target (1024x768 at ss=1)` after arming it on a running game |
| `devres.on` | the world at the device's resolution: `ss` follows `ceil(k)` (`references/ui-layer.md`) |
| `fps.on` | the frame-rate readout (the lever over the FPS counter row), drawn from TA's own glyphs; needs the font the packet carries, which `markown` produces — with only `fps.on` armed the packet reads `font=0/0B` and the readout draws nothing |
| `scaffold.on` | the scene-depth scaffold overlay; a debug instrument that tints every tall feature purple. Not in the arm set |
| `tagpu_<pass>.ab` | a one-frame capture of the presented frame as `tagpu_<pass>_vk.ppm` — `references/measuring.md` |

Read `tacli log <i> -g '^vk:'` for the window, the device, the swapchain and the census.
**An in-process map change brings the whole lane down and back up** (`vk: render thread stopping
- down`, then a fresh `vk: swapchain`): a second map is a fresh lane, its atlases rebuilt at
that map's size.

## Plumbing and safety

| lever | when read | what |
|---|---|---|
| `reclaim.off` | attach | **disables a crash fix**: `tagpu_reclaim` defers the engine's model-object and per-level template frees behind the render thread's quiescence. On by default with no arm file; this is the A/B back to the racing build. Read `reclaim: ARMED FreeObjectState@0x45AAA0 -> deferred …` at launch |
| `packet.off` | attach | the frame packet exchange off: **no world pass draws at all**, no camera command is applied (`tagpu_eye.txt` and the wheel do nothing) and no text draws. The A/B lever, not a feature switch |
| `packet.check`, `packet.stress`, `packet.poison` | attach | CRC-32 of every record verified per take; publish on every in-play draw with one-page slots that must grow (the protocol gate's mode); memset the slot handed back so a pointer cached past its frame reads `0xDD` |
| `grow.stress` | attach | every render array that grows with the unit count moves every frame (freed and reallocated at the exact size; the unit pass rebuilds its slot buffers), so a pointer that outlives a move reads freed memory at stock unit counts. Logs `packet: tagpu_grow.stress - every unit-scaled render array moves every frame`. A measurement lever: an allocation per array per frame |
| `lerp.on` | attach | smooth motion: history-based interpolation of a unit's piece pose between two published ticks (`research/notes/smooth-motion.md`) |
| `posecrc.on` | attach | the gate oracle for it: a CRC of the pose fields, to prove the sim untouched |
| `ghost.on` | live | the translucent building preview at the placement cursor and on queued sites; `alpha=<f>` (default 0.40). A play default; off the bench line because its posed draws perturb a measurement |
| `worldphase.off` | attach | **the world phase off**: the four call-site redirects inside `DrawGameScreen` that bracket the engine's two world spans are not installed, so engine world draws are no longer stamped and `publish` refuses none of them — the lever for any A/B that needs the engine's own world pixels ON THE SCREEN rather than only in the shot. Read `gui: ARMED … worldphase=1` at launch; `worldphase=0` means unarmed — it fails open, so unarmed costs nothing but the leak. Heartbeat `world=<stamped>/<refused>/<live>` |
| `curs.off` | attach | the engine's own contextual-cursor behaviour at `Interface Type=1` back (the DLL otherwise patches the cursor on at any type; `curs: ARMED — contextual cursors on at any Interface Type`) |
| `nano.off` | live | the build-state (nanoframe) look off: a unit under construction draws unstaged, as if finished, with no wire |
| `subpix.off` | live | sub-pixel unit motion off |
| `overlay.off` | per present | the whole per-present GPU pass returns early |
| `menu.off` | attach | the render-options screen off (`references/modules.md`) |
| `hud.on`, `hud.off` | attach | HUD scale (`references/ui-layer.md`) |
| `gui.on`, `gui.off` | attach + live | the UI layer (`references/ui-layer.md`) |
| `weapons.on` | attach | extra weapons (`references/modules.md`) |
| `tagpu_nowarp.on`, `tagpu_title.txt`, `tagpu_defaults.off`, `tagpu_shield.on`, `tagpu_keys.txt`, the `*.trigger` files | | tacli's own plumbing: the pointer-warp suppression, the window title, the defaults opt-out, the firewall, the injected tokens, the on-demand verbs |

## Instruments

| lever | what |
|---|---|
| `cobtrace.on=<TYPE>` | every COB thread start/refuse/return/kill/random to `gamedir/log/tagpu_cobtrace.log` (`references/modules.md`) |
| `posedump.on` | one-shot, self-deleting: dumps the engine's pose fields of the first unit the native pass draws, header `posedump: tick= idx=` |
| `posebake.on=log` | a line per baked model and per material stream. The bake runs without it; the file alone does nothing |
| `surfdump.on` | one-shot: writes the golden source as `tagpu_surf.ppm` and logs `surf: re-read check at draw N: 0 byte(s) of M differ` — must be 0 |
| `surfcheck.on` | continuous: re-reads the golden source on every capture while present; `surf: the continuous re-read check is ARMED …`, silent while 0, a cumulative tally in the heartbeat, the total printed when removed. It roughly doubles the capture cost and holds the surface lock across the compare — arm it for a run, take it away again |
| `ftime.on` | `ftime: vk p50 <ms> p99 <ms> (n=…)`, the GPU frame time |
| `suppress.on` | the render-suppressor (installs at attach) |
| `tracer.on` | the unit-draw tracer (installs at attach) |
| `spxlog.on` | the sub-pixel anchor filmstrip log |
| `gui_census.trigger` | with `gui.on=census pgm`: writes the accumulated residual mask to `tagpu_gui_census.pgm` |
| `tagpu_<pass>.ab` | the one-frame capture (`references/measuring.md`) |

## The heartbeats: what must read zero

| line | every | must read 0 | notes |
|---|---|---|---|
| `packet: pub= skip= overrun= foreign= … viol= pviol= crcbad= nopkt= \| … \| cmd: … \| draws= … \| world: … dup= trunc= relbad= woob= …` | 300 render frames | `viol`, `pviol`, `crcbad`, `foreign`, `commitfail`, `vpwh` (both exchanges); `dup` (the stable-id collision oracle), `relbad` (the engine's `end == begin + (count−1)·0x118` relation), `woob` (wreck records outside the 2048-record pool); `trunc` past each slot's first fill; `layerbad`, `subbad` in the `fx:` segment; `refused` in the `fog:` and `gui:` segments | `skip` is the FRESH gate doing its job; `overrun`/`gap` count only under `stress` or across a level end; `unacked=(0,0)` whenever no wheel gesture is in flight; `hold=1` while `tagpu_eye.txt` is in force; `tps` is 3 × `speed`; `font=` non-zero when text can draw; `levelend=reclaim` names who published the level-end packet |
| `native: vulkan lane handed over frame N: terr= feat= fx= mark= units= posed= sel=N/M selcache= full= fog= bare= out= held=N/Mpx paused= back=` | 300 | `out` past a level's start, `back` | what was handed over, not what was drawn; `sel=N/M` is rects handed to the marker pass of selected units on screen; the fog fields are the fog bound's witness (§"Camera, viewport and fog") |
| `reclaim: def= drn= ovf= … tmpl=<queued>/<freed by the epoch>/<leaked>` | 300 | `ovf`, the third `tmpl` field | a level change logs `reclaim: level teardown: flushed N …` then `reclaim: teardown post: freed N block(s)` |
| `fogwide: …`, `fogwide check: … differ=` | 5 s / 120 ticks | `differ` | a video-mode change logs `fogwide: grid CxR, N KB (N cells) — grown` |
| `gui: twins= …` | 300 | `overflows`, `lost`, `miss`, `reseed` | `references/ui-layer.md` |
| `surf: golden source WxH on the GAME thread -- captured= unchanged= refused= …` | 300 captures | `refused` | `us avg=` is the game-thread cost (55–58 µs at 1024x768) |
| `vk: census: frame N: 6 pass(es) drew and 0 claimed (terr= feat= unit= fx= mark= scaf= gui= fps=)` | 300 | | 6 with `gui=1` is the play set; `mark=0` is the marker pass standing down |
| `mark: bars= cursor= ordtri= ordline= text=… atlas= zoom=` (`log`) | 120 | | the live zoom is readable here; the file lever logs nothing |
| `order: arena= recs= drawn= lines= dots= …` (`log`) | 120 | | `arena=-1` = SHIFT not held |

## Names that do nothing

These appear in older notes and commit messages. No file in the tree reads them; creating one
changes nothing and logs nothing, which is the failure mode to recognise.

`tagpu_glshot.trigger` (and `tacli glshot`, which no longer exists), `tagpu_gldbg.on`,
`tagpu_writeback.on`, `tagpu_posedraw.on`, `tagpu_posefix.off`, `tagpu_posewatch.on`,
`tagpu_poserecon.on`, `tagpu_purevk.on`, `tagpu_selgeom.on`, `tagpu_shade.off`,
`tagpu_rglsl.step`, `tagpu_shadowdump.on`, `tagpu_shadow.ab`, `tagpu_unit.on`, `tagpu_unit.ab`,
`tagpu_restore_<tag>.rgba` without `_vk` (the GL half of the restore dump), `tagpu_<pass>_gl.ppm`,
`tagpu_restorevk.on` (the restorer follows Classic++'s `assets=` knob, which the render-options
screen's `Undithered assets` row writes; `classicpp.cfg=assets=0` is the A/B), `tagpu_zoomedge.off`
(there is no camera setting: the centre range holds whenever our terrain pass owns the ground,
the engine's own range whenever it does not, and nothing else chooses between them).
