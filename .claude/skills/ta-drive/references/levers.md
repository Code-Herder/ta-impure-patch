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
| `mirror.on` | | the store's `edge` key decides it (below) |
| `gui.on` | | |
| `classicpp.on` | | |
| `recorder.on` | | |

Not on the defaults, and why: `owndraw`, `featown`, `fxown` stop the engine drawing what our pass
draws and so hole the golden source (`tacli` still auto-arms them when the pass is armed, because
they save the engine's CPU); `hud.on` changes the look of every screen and is the owner's call;
`scaffold.on` and `fps.on` are instruments.

**`terrown` is the exception and is a default.** It holes the golden source like the other three
— the viewport arrives key-filled, with no terrain — but it is an **optimisation, not a
requirement**: the play defaults and the play defaults plus `tagpu_terrown.off` render the same
picture (`build-facing` at 1024x768: 0.03 % exactly-black viewport and 8898 distinct colours
either way), and with it off `tacli shot` has terrain again. **`tacli arm <i> terrown.off` is the
arm for a reference-quality capture.** It keeps the **zoomed-out fog**: without `terrown`'s
fog-overlay detour the packet's publisher ticks `tagpu_fogwide` in the draw's `after`, so every
frame still has the whole map's fog source (`fog=WxH`).

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
  fog=WxH out=N held=N/Mpx paused=N back=N reachheld=N floor=F nopieces=N` — what was handed over to the Vulkan passes, not what
  they drew; the fog fields are below. A model that will not bake (over 256
  pieces or 49 152 vertices) logs a `posebake: REFUSED` line and draws nothing. Nothing reads
  `gamedir/hires/`: a `.glb` there changes nothing, because replacement meshes are not in this build.
- **`terr.on`** owns the terrain and the fog overlay; `key=N` moves the palette index the engine's
  viewport is filled with (254). Without `terrown.on` it emits nothing and its `terr:` line says why: our terrain is opaque
  over the whole viewport, so drawing it with no key fill to invert against would hide every
  engine overlay and still look right. Arming `terr.on` after launch does nothing visible —
  relaunch. Expect `terr: atlas built
  2176x<h> for <n> tiles`, `terr: height grid WxH uploaded` (without it Classic++ terrain draws
  unlit), `terr: tile map WxH copied for level N (serial S, K cells past the atlas)` once a level,
  and `vk: terr: the Vulkan edition is up …`. Under Classic++ `assets=1` it draws the
  restored atlas. Its heartbeat, every 60 frames: `terr: grid=CxR tile0=(x,y) frac=(fx,fy)
  map=WxH area=WxH cells=N zoomvp=WxH mirror=1|0 junk=K atlas=…`, where `cells` is the whole grid
  (`C × R`, one instance each, drawn or not) and `junk` the tile map's cells whose tile is past the
  atlas, counted once at the copy.
- **`feat.on`** takes the draw only while `native.on` carries `wrecks` (3D wreckage is drawn from
  the same leaf); otherwise it logs `nothing emitted: native.on needs "wrecks"` and `atlas=0`.
  Heartbeat under `log`: `feat: rect=… anchors= flat= tall= … 3dwreck= … outside= trunc= -> body=
  shadow=` every 60 frames. **`outside=` must read 0** — cells the frame's rect asked for past the
  rect the packet's anchor table covers; **`trunc=` must read 0** — frames drawn from an anchor
  table the publisher cut short (it drops the rows farthest from the view's centre first; the
  densest stock map fills 19 % of it at 4K). Fixture `feat-forest`.
- **`fx.on`** is called from inside the native pass, so it needs `native.on` (any filter). The
  engine skip follows the file live: `fx.on=off` restores the engine's effects within 30 frames.
  Heartbeat `fx: proj= expl= … lines= sprites= flashq= models= runs= (lost= why= of cap )` (the
  table below). The effects' 3D models are drawn by the unit pass, and a frame whose models the
  unit pass does not draw shows none of its effects. A frame the unit pass will not draw for a
  reason known before the gather (the scaffold armed, a draw the hand-over does not carry, a
  texel mirror not there yet, a slot the models' blocks would not fit) takes back only the
  records with a model, and every other effect draws. Fixtures `fx-mix`, `fx-lasers` (burns out in
  tens of seconds), `fx-rockets` (stays alive).
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
| order lines | `order.on` **and** SHIFT physically held (`down:shift` … `up:shift`; a bare `shift` is a tap, down and up) **and** an order that does not complete — `move` completes and takes its markers with it; `patrol` never does |
| order triangles | the route dots — only for the **hovered** unit; `pmove:x,y` onto it from a fresh roster, at game speed 1 so it does not walk away |
| labels | `+showranges`, typed into the chat |
| cursors | a **held** drag (`pmove:x0,y0`, `down:lbutton`, `pmove:x1,y1`, capture, `up:lbutton`) or a build placement. The one draw with the fog off: over fogged ground it stays undarkened, which is what an A/B of the post-fog draw needs on screen. `mark.on=nocursor` hands both back to the engine and draws neither |

`scenarios/marker-mix.json` is the fixture that opens all of them except the labels. It has
three tanks at 30, 65 and 100 % health and a Commander, on open ground
with water and the fog band in view. Its description gives the key sequence. Health bars still
need `damagebars`, and the damaged tank smokes, so turn `fx`/`sfx` off for a pixel A/B.

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
| `zoom.on` | attach | the wheel, `tagpu_zoom.txt`, the minimap view rectangle, the scroll step (`0x41CEAA`), and the camera's centre range. Logs `zoom: scroll step armed at 0x41CEAA -- the scroll poll's step is divided by the level; ScrollSpeed main+0x1434D is never written` and `zoom: ARMED (minimap rect 0x466B70 x2, camera centre range 0x41C3C0 + target clamps …)`. It also installs the `0x498DA0` mouse->world repair, which is what the wheel and the file need; `vpwide.on` installs that repair too, so either one un-pins the level. With neither: `zoom: PINNED AT 1.0 — the 0x498DA0 mouse->world repair is not installed` |
| `tagpu_zoom.txt` | every frame | a bare float 1/64–8.0 (0.015625–8), drawn no lower than the map's zoom floor; written atomically (temp + rename); pins the level and disables the wheel while present; does not move the camera |
| `tagpu_eye.txt` | every in-play draw | what `tacli eye X Y` writes; the packet heartbeat's `hold=1` says it is in force; `tacli eye <i> --release` removes it |
| `wheel.off` | live | the wheel does nothing; `wheel.off=off` removes it. Nothing arms the wheel separately: it comes with the mouse->world repair (`zoom.on` or `vpwide.on`) |
| `vpwide.on` | attach | widens the rect the engine addresses to what the zoom shows, so ring clicks and band boxes land at zoom < 1. Writes `main+0x37E27..0x37E33`. Logs `vpwide: ARMED (mouse->world 0x498DA0, surface …)` and `vpwide: true viewport rect verified (128,32 896x704)`, `vpwide: viewport rect restored to 1x` at 1x. `zoom.on` alone logs `vpwide: mouse->world repair only (0x498DA0) —` |
| `mirror.on` / `mirror.off` | live (30 frames) | the map edge: the world reflected past the map, or black. On the play defaults, where the store's `edge=mirror\|black` decides it; under `tagpu_defaults.off` (every tacli control launch) it is **black** unless `mirror.on` is armed. The render thread logs `native: map edge = mirror` or `= black` on every change, the one line to read |
| `fogwide_check.on` | live | the oracle: `fogwide check: CxR compared=N of cells=M skipped=K differ=N engine-nonzero=N truelos=0|1 mirror-compared=N mirror-differ=N` every 120th tick. It rebuilds the engine's own window from our packed source and compares it with the engine's grid, **`differ=0`**, and checks the shared expansion (`tagpu_fog_entry`) against that build, **`mirror-differ=0`**. `skipped` is the border lines where the engine's literal completion row or column and ours differ, both left out — 0 for an eye in the engine's own `[0, extent − W]`, non-zero near the map's edge in the centre range |

Driving the camera at a zoom other than 1:

- **The camera keeps the view centre on the map, at every zoom**: the eye ranges over
  `[−W/2, extent − W/2]` (W, H the true viewport, 896x704 at 1024x768), so a map edge can reach
  the middle of the screen. `extent` is the scroll extent `main+0x1422B`/`+0x1422F` — the tile
  map less 32 px wide and 128 px tall (Two Continents: tile map 10752x12800, extent 10720x12672),
  where stock's view ends and where the mirrored edge folds. That range holds only while `terr.on`
  draws the ground; without it the eye stays in the engine's own `[0, extent − W]`. `tacli eye`
  is clamped into the range in force, so `tacli eye <i> -99999 -99999` puts the map's NW corner at
  the view centre and `99999 99999` the extent's SE corner (`8768 8864` on Painted Desert at
  1024x768). The terrain heartbeat prints both sizes, in 32-px tiles: `terr: … map=289x292
  area=288x288 …`. A right-click past the edge orders a move to the nearest point of the extent.
- **The mega map is the other wheel** (`wheel=` in `impure.cfg`, the *Mega map wheel* row):
  Ctrl+wheel by default, the plain wheel under `wheel=megamap`. A mega notch back cuts to the floor
  with the view centre on the map's centre (and follows the floor if it moves while engaged); any notch forward while engaged cuts back to the level it
  left, holding the point under the pointer (`megazoom=mouse`) or centring it (`megazoom=tadr`; the in-game panel's *Mega map zoom in* row). A cut changes one presented frame, and the same-way notches behind it in a spin are dropped. A press on the world right behind a mega notch is dropped too, until the new view has been drawn (a script's `wheel` then `pclick` gets `TAGPU_DELIVER_DROPPED`), and a mega notch with a world button held does nothing. Inject it with `keys <i> down:ctrl pmove:X,Y
  wheel:-1 up:ctrl` (the shield must be on). The store counts only under `--defaults`; a tacli
  control launch always has Ctrl+wheel as the mega map.
- **The zoom floor follows the map**: `min(0.25, fit)`, the level at which the whole scroll
  extent fits the true viewport, bounded so the widened rect's four edges stay int16. The game
  thread logs it when it changes (a level's first draw, a resize): `zoom: floor 0.0346 (fit 0.0346)
  on 896x704, extent 20448x20352, map 20480x20480 - bound by fit` (Seven Islands at 1024x768 by
  the arithmetic; not yet read off a run) — the term is `fit`, `lparam`,
  `1/64` or `0.25` (a map the screen covers at 0.25x). Read the floor there or off the `native:`
  line's `floor=`, never assume 0.25. The wheel and the mega notch stop at it; `tagpu_zoom.txt`
  and a Follow below it draw at it. Edge and arrow scroll move the screen at the 1x rate at
  every level, the floor included: the scroll poll's step is divided by the level at `0x41CEAA`,
  and `ScrollSpeed` `main+0x1434D` keeps the player's value.
- **The unit hit test is guarded at every level**: a unit whose position is more than about 32 000
  px from the eye (or that high) answers "not hit" to hover and click, and the game thread logs
  `zoom: hit guard - N unit hit test(s) answered "not hit" so far, …` at most once a second. It
  reads 0 in ordinary play; a count there is a far or off-map aircraft, not a fault.
- **Zoomed out, units turn into strategic icons** (no lever; the module is
  `research/notes/modules/icons.md`). Three `impure.cfg` keys, read live, no menu row:
  `iconscale` (100, 75, 125, 150, 200: size, percent of BAR's), `icondist` (100, 50, 75, 150, 200:
  where icons take over, percent of BAR's distance) and `iconfade` (33, 0, 67: the fade band; 0 is
  a hard switch). Like `wheel=`, the store counts only under `--defaults`; a tacli control launch
  draws at 100/100/33. With those, 1x shows no icon on any frame up to about 2600 px tall, and at
  1920x1080 COMBAT starts to fade in below z 0.409 and is opaque below 0.274 (the module's table
  has every class). Read `icons: on= units= icons= set= hidden= cargo= dots= drop= base= zS= z=
  alpha(combat)= alpha(dot)= edges=… by-class=… us=` every 300 frames: at 1x `icons=0 hidden=0`;
  `zS=` is the level the frame spans BAR's distance at (`frame height / 4970`, 0.2173 at 1080);
  the nine-field lists are in class order UNKNOWN, COMMANDER, FACTORY, BUILDING, AIRCONS,
  AIRCOMBAT, AIR, CONS, COMBAT. Icons need the unit pass and the marker pass both drawing (`on=0`
  otherwise). A click on an icon whose model is hidden is answered by the icon's circle on the game
  thread: `zoom: icon pick - N hit(s), M miss(es) so far`, at most once a second. Radar contacts
  show as the minimap's dot under the same rule (`dots=`) and are not pickable in the world view.
- **A mod's TADR icon set replaces the shapes** where the install ships one (no lever; it is read
  once a process from the files `MegaMapConfig` names in the ini the exe's `0x5098A3` string
  names — Mayhem, ProTA, TA Zero and Twilight do; retail and the compat setups' Escalation do not). The
  log says which, once: `icons: mod set <folder> from <ini> (<MegaMapConfig>), default|custom
  mode, N picture(s) loaded of M, … skipped: …` or `icons: no mod set - …`; the art line then
  counts `N mod set picture(s)`. Each level with a set logs `icons: mod set by type - E map
  entr(ies), P of T type(s) by a picture, U UNKNOWN; CTRL_W= CTRL_B= CTRL_F=; <category>=<types>`
  (a custom set's categories, `absent` for one the game does not have). `set=` on the heartbeat
  counts the icons drawn as set pictures. A set picture is drawn at `k = round(base/10)` (1 at
  1080p, 2 at 4K) and switches at its own drawn size, so it is not the class table's edge.
- **A wheel round trip does not land on 1x** (x1.163 then x0.877 is 1.020). Re-anchor by writing
  `1.0` to `tagpu_zoom.txt`, then deleting it. Deleting the file hands over, it does not reset;
  notches sent while the file is present are dropped and logged (`zoom: wheel … ignored -
  tagpu_zoom.txt is in force`).
- **Jump the camera with the minimap, not the arrow keys.** A *held* left button on the minimap
  lands the eye exactly on `world − (W/2, H/2)` before the clamp:
  `keys <i> mouse:10,0 down:lbutton`, peek `+0x1431F`/`+0x14323`, `keys <i> up:lbutton`.
  `pclick:` alone does not work here; the jump wants the button held across a frame.
- **`tacli eye` rebuilds the fog.** The engine rebuilds its screen fog grid only when its
  is-current bit (`main+0x14281` bit 3) is clear, and the game thread's apply clears it whenever
  it moves the eye, as the engine's own eye writers do: measured with the sim paused under
  `ARMOPT`, one `tacli eye` jump made exactly one rebuild and no frame drew the old grid at the new
  eye.
- **Dialogs drawn inside the viewport keep 1:1 clicks at every zoom** (`ARMOPT`, `EXITMENU`,
  `YESORNO`, the preferences screens); `SHARE.GUI` is the known gap, and `ui press <gadget>` is
  the fallback there.
- **The mirror reflects what the world passes draw.** While the feature pass draws the map's
  features, the edge mirrors ground and features together or not at all; while it draws none
  (`feat.on` unarmed, `feat.on=passive`, or `native.on` without `wrecks`), the map shows no trees
  either and the ground mirrors alone. A gather that bails draws no features on the map or past
  it that frame (the engine's own feature draw reaches only the reference surface, gpu-status
  §2.81), so no bail gives a bare mirror beside a wooded map. Its features are the map FILE as its
  TNT lays it out — never a scenario's or the `.ota` schema's, never a wreck, never what play or a
  saved game did to the map: a tree reclaimed before a save is still in the mirror after the load.
  The list is built on the loader thread inside LoadMap (`packet: LoadMap (loader thread T, level
  gen N): the map's own features from its TNT: K over WxH (v2, a new game|a saved game; …) in X
  ms`), adopted by the game thread at the level's first in-play draw (`packet: level gen N: the
  map's own features adopted from the load: K`), and carried only while the feature pass draws
  features with the edge on `mirror`. While it does, the ground waits for that table: nothing
  mirrors until `feat: mirror: the map's own features taken for level N: K of K over WxH` has
  logged — `0 of 0` on a map with no features, whose ground then mirrors. A build that fails
  (`packet: LoadMap: the map's own features NOT taken: …`) leaves the level with no table and the
  edge black; every LoadMap empties the hand-over first, so no earlier load's table stands in.
  Under GDI nothing is built. Heartbeats: `terr: … mirror=1|0` (the switch the grid is drawn
  under; the line counts no off-map cells), `feat: … |
  mirror: map=K anchors= body= shadow=` under `log` (`map=-1` while the pass holds no copy), and
  `packet: … mapfeat=K carried=N`, where `carried` stops climbing once the feature pass holds the
  table.
- **An edge=black A/B is the control launch as it is**: `tagpu_defaults.off` already draws black,
  so arm `mirror.on` only on the side that should mirror.
- **Edge scroll is an equality on the outermost pixel** (`x == 0`, `y == 0`, `x == W−1`,
  `y == H−1`), at every zoom on all four edges.

The fog source (`tagpu_fogwide.c`) is the whole map's fog, two bits a 32-px cell (never explored;
out of LOS in true-LOS mode), packed on the game thread and expanded in every pass's shader as the
engine expands its own grid; its heartbeat is `fogwide: WxH source, N KB, packs=N in 5.0s = R/s
ticks=N pack=…/… us (mean/max)` once per five seconds of wall time. **`packs=0` is not a fault at
LosType 12** (`--los 0`) on a fully mapped scene: nothing changes, so nothing repacks — read the
word at `*0x511DE8+0x14281` before chasing it. Fog has no off switch: a frame with no source (`fog=0x0`) draws no unit but the watched player's own and no wreck.

The native heartbeat's fog fields: `fog=WxH` is the source the last frame handed over (`0x0` with
none). **`out=` must read 0** — frames whose drawn slab was not inside the packet's tables'
reach (`reach_lo`/`reach_hi`); **`nopieces=` must read 0** — units inside a frame's slab that the
packet carried outside that reach, so without their pieces and not drawn; `held=N/Mpx` is the
frames whose drawn eye the reach bound held back and the largest hold, never more than the
displacement the gesture has posted and the game thread not yet applied, less the lead the reach
carries (a quarter of the view a side) — near 0 when the game thread keeps up, more when it lags;
`paused=` is the frames drawn at the previous frame's level, so the view would not move against
the gesture or while a mega map cut waits for its packet; **`back=` must read 0** — frames that moved against it anyway. `out=` and
`nopieces=` count only frames the Vulkan lane presented, so the lane's
bring-up and a swapchain rebuild add nothing to them.

## Classic++

| lever | when read | what |
|---|---|---|
| `classicpp.on` | polled twice a second | the master arm: restored true-colour terrain, features, effects, unit textures **and the UI** (sidebar, minimap, top bar, shell), the lambert lighting and the cast shadows. Flips live; arming mid-play restores what is on screen |
| `classicpp.cfg` | live (re-read on the poll when its mtime or size changes) | the knobs: `sun=AZ,EL` or `sun=off`, `unitsun=AZ,EL`, `amb=A`, `assets=0|1`, `light=0|1`, `shadows=0|1|2`, `shadowsun=AZ,EL`, `penumbra=K`, `shadowlen=A,B` or `off`, `shade=S`, `terrainshadow=0|1`, `shadowres=N`, `airshadow=len|physical|drop`, `aniso=N`. Written by `tacli arm <i> 'classicpp.cfg=sun=off shadows=0'`, removed by `classicpp.cfg=off` |
| `restoreglsl.on` | when the restorer starts (arm before launch) | the restorer core's two knobs, in the file that keeps the name it had: `log` (a line per batch) and `budget=MS` (GPU milliseconds per frame, 0.5–100, default 12). Nothing else is read |
| `restoredump.on` | after each queue drains (the terrain's once every neighbourhood is painted) | writes `tagpu_restore_<tag>_vk.rgba` (`.mips` for the units), its source (`.base` for a world atlas, `.r8` for the UI's), `.pal` and `.idx` per job, and the terrain's map `tagpu_restore_terr_vk.map` — the byte oracle, checked by `tools/restore-dumpcheck.py` (`references/measuring.md`) |
| `restorefault.on` | once per process, at the first restorer frame (arm before launch) | drives the restorer's guard: `probe` spoils one byte of the launch self-test, `crash` faults inside a restorer call, `lost` has the seam report a device loss with restorer work in flight. Each turns the restorer off for this device and driver, `crash` and `lost` by relaunching the game, so **remove the file and the record afterwards** (below). Two more drive the terrain's per-tile fallback and record nothing: `nbfit` (the neighbourhood atlas does not fit) and `nballoc` (the device refuses it — on the first map after the terrain pass comes up only) |

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
  shot: the word is kept in the instance's registry, so a shadows-off an earlier run of the
  instance left shows up in its control launch.
  **Arming `classicpp.cfg=shadows=0` before a `--defaults` launch makes the store clear those bits**
  (the word reads `0x23`), and removing the cfg later does not bring them back: launch with the key
  at its default and switch shadows off live. `Shading` (bit 5) is the store's under `--defaults`
  too, so a registry value does not gate it there; arm the passes by hand (the default arm set in
  `SKILL.md`) to test the option, and peek the word to confirm.
- `sun=off` is exactly `light=0`. `light=0` leaves a unit **unshaded**, not Classic-shaded (the
  engine's per-face shade row is the Classic branch's). `assets=0 light=0 shadows=0` is the one
  Classic++ state that is a Classic frame.
- `terrainshadow=1` self-shadows the ground (default 0); at the default sun elevation it is
  nearly invisible, so the fixture for the map is `shadowsun=225,8` on `static-terrain`.
- `aniso=1` for a unit-pass A/B; 4 ships.
- Read the result of a restore in the `restorevk:` lines — `restorevk: <tag>: lazy restore armed
  (…)`, `restorevk: <tag>: done: N frames (… wrap-padded) in B batches, D dispatches in S of F frames = …`
  and, with `restoredump.on`, `restorevk: <tag>: restored atlas dumped to …`. The tags are
  `terr`, `feat`, `fx`, `unit`, `gui` and `pic`, plus the self-test's `probe-base`,
  `probe-pal` and `probe-nbhd`. The fps in the
  `done` line is the rate the game held during the restore. The first job of every launch is abandoned by the
  startup reset and restarted; the `done` line is the second job's. Wait for it before a parity
  capture.
- **The terrain restores by neighbourhood** where its atlas fits: `vk: terr: the terrain restores by
  neighbourhood - K keys in a WxH atlas: … MB against half of the … MB the driver says is free`
  (or `… a quarter of the … heap: no memory-budget query here`), else `… does not fit … - the
  terrain restores per tile`. Its job is fed, so it has no `done` line: wait for `vk: terr: every
  neighbourhood painted here - K keys, … ms since the job began` (about 20 s on Two Continents)
  before a whole-map capture. The first screen is painted within about 0.1 s.
- **Nothing restores before the self-test's line**: `restorevk: self-test passed on <vendor:device:driver>:
  N bytes within 0 level(s) of the CPU reference, computed in … ms`, once per device per process. A
  `self-test FAILED` or `could not be run` line means the art stays dithered.
- **The restorer can be off by record.** `restoreguard:` lines say why: the crash filter installed
  at the restorer's first bring-up, a record naming this device (`recorded off for …`), a record
  dropped for a new driver, card or build. The key is `vendor:device:driver:build`, the build being
  the DLL's commit, so **every rebuild from a new commit drops the record** and tests again.
  The record is `restoreoff=` in the instance's `impure.cfg`, or `tagpu_restore_off.txt` in the
  gamedir under `tagpu_defaults.off`; `tagpu_restore_crashed.txt` is the marker a crash leaves for
  the relaunch: read at the first bring-up only, dropped when it names another build or device,
  and deleted once the disk holds the record (`the crash marker is deleted`).
  Delete all three by hand to start clean. The render options' *Undithered assets* row reading
  "Off (driver)" is the same record, and picking On from it or from Off retries; under
  `tagpu_defaults.off` that row is greyed, so the file goes by hand.
- **A relaunch logs `this process is a relaunch … it waited for the crashed process to end`**:
  it holds at attach until the old process is gone, since TotalA.exe exits silently while another
  copy holds its semaphore. `tacli ls` then shows the new pid; `tacli` commands keep working.

## The Vulkan lane

The Vulkan backend is the renderer on every launch. **`tacli arm <i> gdi.on`** before the launch
forces the GDI backend (`cfg: … renderer GDI (tagpu_gdi.on)`), the other lane, on which nothing
of ours draws, no golden source is captured, and every `tacli` verb except `eye`, `wheel` and
`gui` still works. `gdi.on=off` puts Vulkan back; it is read at attach only.

| lever | what |
|---|---|
| `vk.on` | not needed to run the lane; read for `color=r,g,b`, the clear colour (black by default; `255,0,255` makes every undrawn pixel a magenta sentinel) |
| `vk.off` | **ignored** on the Vulkan backend (the log says so) |
| `tagpu_vk.gpus` | the GPU row's device list (one launch behind); the choice is `gpu=` in `impure.cfg` — `references/modules.md` |
| `ss.off` | the 2x supersample off, and the lever over the Supersampling row. Live: the world target (`gw*ss x gh*ss`) is rebuilt on the next frames — `vk: world: frame N: 1024x768 target (1024x768 at ss=1)` after arming it on a running game |
| `devres.on` | the world at the device's resolution: `ss` follows `ceil(k)` (`references/ui-layer.md`) |
| `fps.on` | the frame-rate readout (the lever over the FPS counter row), drawn from TA's own glyphs; needs the font the packet carries, which `markown` produces — with only `fps.on` armed the packet reads `font=0/0B` and the readout draws nothing |
| `scaffold.on` | the scene-depth scaffold overlay; a debug instrument that tints every tall feature purple. Not in the arm set |
| `chaincheck.on` | live, re-read every 120 frames: the mip chains' oracle, in the game's world only (never the wiki's stage). Every 120 frames each pass checks every cell of its chains on the GPU and 16 base cells, every level, against the C reference: `vk: terr: chain check: base cells=16 cpu-differ=N gpu-differ=N/S | rest gpu-differ=N/S job=G painted=P mode=nbhd|tile` (or `| rest none`) and `vk: feat: chain check: base slots=16 cpu-differ=N gpu-differ=N (seen S) | twin gpu-differ=N (seen S), cells=C overflow=K` (or `twin gpu-differ=none`). Every `differ` must read 0; `S` is the cells checked; `cpu-differ=skipped` means the atlas, palette or allocation moved since the copy, `outside` a cell past the hand-over's atlas. The terrain's `rest gpu-differ` reads 0 across a restore job made again on the same request as well (the chain is kept until the new job's first paint, which reduces it whole). Below a texel a target pixel (zoom × ss < 1) the terrain and features draw from these chains |
| `tagpu_<pass>.ab` | a one-frame capture of the presented frame as `tagpu_<pass>_vk.ppm` — `references/measuring.md` |

Read `tacli log <i> -g '^vk:'` for the window, the device, the swapchain and the census.
**The mip chains say once per device** if the device will not reduce: `vk: chains off: <reason> -
the far zoom draws the atlases at level 0` (then every zoom draws level 0 and shimmers below
`1/ss`). On a map they log `vk: terr: chain base WxH (5 levels) reduced, N cells, atlas serial S`
once per map and palette move, `vk: terr: chain rest WxH cleared|kept (level 0 stands until the job's first paint)|reduced whole N cells, job G` per
restore job, `vk: terr: the restored chain does not fit: … - the far zoom draws the base chain
under Classic++` when D10 refuses it, and `vk: feat: chain cells laid out for atlas generation G
(repack R) - N entries, C of 8192 cells, overflow=K` per atlas layout; `overflow=` should read 0.
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
| `scratch.stress`, `scratch.nogrow` | attach | the composite scratch frame's bound (an engine fix, always on): `stress` regrows the frame at every writer call to exactly what the call asks and frees the old one with its two plane pointers set to `0x80000000`, where nothing is mapped, so a frame pointer held across a writer faults at any plane access; `nogrow` refuses every grow, so an oversized unit takes its writer's fallback; both together send every writer to its fallback. Logs `enginefix: composite scratch lever: …`, and each grow or refusal as `enginefix: composite scratch grown|refused (<reason>) for <writer>: N px in R rows asked, A held, P now (n so far)`, a writer's first 16 lines, then every 1024th. A measurement lever |
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
| `input.stall` | per key batch | a test lever: a batch taken while it exists has no event acknowledged (`input: batch <id> taken (tagpu_input.stall: no acknowledgement): …`), so it reports `waiting` until tacli abandons it -- the abandon path on demand. Remove it before the next batch |
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
| `recorder.on` / `recorder.off` | a play default: the demo recorder records every skirmish and native multiplayer game to `gamedir/replays/`, and each new recording prunes that folder to the newest 50 (`keep N` in the file changes the count). On a bare tacli launch arm `recorder.on` to record; under `--defaults` arm `recorder.off` to stop. Read at each level's first iteration, so arm it before the level starts (`references/modules.md`) |
| `tagpu_<pass>.ab` | the one-frame capture (`references/measuring.md`) |
| `dmgdelay.on=K` | **test only, never on a player's instance**: every hit this peer sends leaves K ticks late (default 30, at most 3600), so a hit lands after its victim's slot has been reused; logs `enginefix: stale hits: TEST LEVER …` and a `take <slot> (first-free <slot>) at <GameTime>` line per local create. Arm it on the attacking peer (`references/modules.md`, *Stale hits*) |
| `turbo.on=K` | **test only, a tacli test launch**: K ticks a frame (1..5, empty for 1) once `turbo on` reaches `tagpu_replay.trigger`, none while the recorder's ring is more than half full; `turbo: k=K at 0x49692D` at attach (`references/modules.md`, the demo recorder) |
| `rec.decline` | **test only, a tacli test launch**: every recording's stream codec declines blocks 2, 7, 12, … by the encoder's first decline branch, so the records block and the restart after it are exercised (the header names the lever whenever the file is present, even where the gate refuses it: the end block's declined count says whether blocks were declined) (`recorder: tagpu_rec.decline: the codec declines stream blocks 2, 7, 12, ...` at attach); tareplay's `codec-decline` and `stock-codec-decline` fixtures set it. Read at attach |
| `presentcap.on=N` | **test only, a tacli test launch**: at most N presents a second (10..60); `frame cap: a test launch's N fps present cap (tagpu_presentcap.on)` |
| `ghostq.off` | **test only**: the ghost-commander queue holds and notes nothing, so a create refused during the load waits for the round robin as on stock; the dirty create's position still applies, which is how that half is measured alone. Logs `enginefix: ghost commander: TEST LEVER …` |

### The Mods screen

- `tagpu_modsui.dump`: touching it writes `tagpu_modsui.json`, the screen's state with the hit map of the frame last drawn in client pixels (`open`, `holds`, `drawn`, `tab`, `rows`, `opts`, `sw`, `mutators`, `hits`, `note`, `running`, `chosen`).
- `tagpu_modbase.on`: its contents are a download server's address, standing in for every catalog entry's `base_url`; read at each open of the Mods screen, it also has Explore and Updates offer every entry.
- `tagpu_modcatalog.on`: its contents replace the catalog packed in the DLL, read once at attach; the patch pack stays the DLL's. For test mutators.
- `tagpu_hudrefuse.on`: the replay HUD's and wiki's pass refuses at its build, as it does with no address space left.
- `tagpu_passrefuse.on`: the UI pass is refused its resources as a full GPU refuses it, once (the file is consumed) or on every frame when the file says `keep`; written into a live game's folder, it takes effect within half a second. Once, the lane comes down and back; `keep`, the session goes to GDI.
- `tagpu_vkfull.on`: the driver's own no, `VK_ERROR_OUT_OF_DEVICE_MEMORY` from `vkAllocateMemory`. A number K (or nothing, 0): K allocations pass, one is refused, the file is consumed. `keep`: every one. `wiki`: only while the wiki's stage is being prepared. An allocation is what it needs to meet, so a rebuild is the usual way in: write it beside `tagpu_passrefuse.on`.
- `tools/mods/modmp.py [--only dropin|gameplay|cosmetic|stock|older|olderhost] [--old-dll PATH] [--film FILE]`: the multiplayer fingerprint, in two-player games on Wine. `tagpu_modprint_stock.on` in a game folder sends stock's 21 bytes of player data, with no key and no print.
- `tools/mods/modsscreen.py [--only flow|real|boxes|menus|mutators|hosted] [--display :N] [--software]` plays the screen on a private display and holds the run lock. `hosted` alone downloads from the real hosts, so it runs only when named.

## The heartbeats: what must read zero

| line | every | must read 0 | notes |
|---|---|---|---|
| `packet: pub= skip= overrun= foreign= … trunc= refill= viol= pviol= crcbad= nopkt= \| … \| cmd: … \| draws= … \| world: … dup= trunc= relbad= woob= …` | 300 render frames | `viol`, `pviol`, `crcbad`, `foreign`, `commitfail`, `vpwh` (both exchanges); `trunc` (a frame published cut: the slot could not grow to the fill); `dup` (the stable-id collision oracle), `relbad` (the engine's `end == begin + (count−1)·0x118` relation), `woob` (wreck records outside the 2048-record pool); the `world:` segment's units, wrecks and anchors `trunc`; `layerbad`, `subbad` and `lost` in the `fx:` segment; `refused` in the `fog:` and `gui:` segments; `recodd` in the `fog:` segment (a draw that cleared the engine fog grid's is-current bit without building it) | `skip` is the FRESH gate doing its job; `refill` counts fills done again after the slot grew under them, a few at a level's start; the segments' counts are per publish, from the fill that was published; the `world:` pieces `trunc` reads at most the packet's `trunc`; `overrun`/`gap` count only under `stress` or across a level end; `unacked=(0,0)` whenever no wheel gesture is in flight; `hold=1` while `tagpu_eye.txt` is in force; `tps` is 3 × `speed`; `font=` non-zero when text can draw; `levelend=reclaim` names who published the level-end packet |
| `fx: proj= … -> lines= sprites= flashq= models= runs= (lost= why= of cap ) atlas=` | 60 | | `models=` the effects models drawn this frame, `runs=` their rasterised runs, `cap` the frame's bound on them; `lost=` models taken back with their records and `why=` the rasteriser's last refusal: 1 a model the packet did not carry, 2 a texel the unit atlas has not painted yet (its allowance: a few frames at a level's start), 3 no room (a frame's runs past 131 072, a face past 65 536 scanlines, or a rasteriser buffer that would not grow). A `lost=` that stays non-zero with the runs well below 131 072 is a fault. The packet's own `fx:` segment carries `models=N/Vv lost=`: the models carried, their vertices, and the models the publisher could not carry. A frame whose models the posed or the unit pass dropped says so once, `posedraw: N effects model(s) could not be carried …`, `unit: … the effects are not drawn, the units are`, or `unit: the slot would not take … the models are left out until every slot takes them`: that frame draws its units and none of its effects, and after the last one the frames that follow take back only their model records until every slot holds them |
| `native: vulkan lane handed over frame N: terr= feat= fx= mark= units= posed= sel=N/M selcache= full= fog=WxH out= held=N/Mpx paused= back= reachheld= floor= nopieces=` | 300 | `out`, `back`, `nopieces` | what was handed over, not what was drawn; `reachheld=` the frames the reach ladder held at a rung, `floor=` this frame's zoom floor; `sel=N/M` is rects handed to the marker pass of selected units on screen; `fog=` is the fog source's size and the rest the reach bound's witness (§"Camera, viewport and fog") |
| `reclaim: def= drn= ovf= … tmpl=<queued>/<freed by the epoch>/<leaked>` | 300 | `ovf`, the third `tmpl` field | a level change logs `reclaim: level teardown: flushed N …` then `reclaim: teardown post: freed N block(s)` |
| `fogwide: …`, `fogwide check: … differ=` | 5 s / 120 ticks | `differ` | a video-mode change logs `fogwide: grid CxR, N KB (N cells) — grown` |
| `vk: terr: chain check: …`, `vk: feat: chain check: …` (`chaincheck.on`) | 120 render frames | `cpu-differ`, `gpu-differ`, `rest gpu-differ`, `twin gpu-differ` | the lines are in *The Vulkan lane*; `skipped` and `none` are not failures |
| `gui: twins= …` | 300 | `overflows`, `lost`, `miss`, `reseed` | `references/ui-layer.md` |
| `surf: golden source WxH on the GAME thread -- captured= unchanged= refused= …` | 300 captures | `refused` | `us avg=` is the game-thread cost (55–58 µs at 1024x768) |
| `vk: census: frame N: 6 pass(es) drew and 0 claimed (terr= feat= unit= fx= mark= scaf= gui= fps= hud=)` | 300 | | 6 with `gui=1` is the play set; `mark=0` is the marker pass standing down; `hud=1` while the replay HUD draws |
| `mark: bars= sel= selover= icons= iconover= cursor= place= placered= placetake= ordart= ordline= text=… atlas= zoom=` (`log`) | 120 | `iconover` | the live zoom is readable here; `icons=` counts the strategic icons' quads, `iconover=` those the bucket refused; `ordart=` counts the order art's quads (route dots and waypoint sprites); `place=` counts the placement sites drawn, `placered=`/`placetake=` those of them red and yellow; the file lever logs nothing |
| `icons: on= units= icons= set= hidden= cargo= dots= drop= base= zS= z= alpha(combat)= alpha(dot)= edges= dotk= by-class= us=` | 300 | `drop` | the strategic icons (§"Camera, viewport and fog"): `set=` icons drawn as a mod set's picture, `hidden=` models not drawn under an icon, `cargo=` units carried by one, `dots=` radar contacts (a dot, or the set's NONE picture), `us=` the frame's decision + the sort and emit, in µs; `on=0` when the unit or the marker pass is not drawing, no type table rode the packet, or the art is not published yet (`icons: art N radar logo frame(s) of M, …` once a process) |
| `order: arena= recs= drawn= lines= dots= …` (`log`) | 120 | | `arena=-1` = SHIFT not held |
| `… \| hits: bad= bare 09= 0b= over= young owner= by= slots= tables= out 09= 0b= B= stockB= unk= in 09= 0b= owner=A/R by=S/U/R dead= gate= copy exact= bound= unk= held= fallback= holdfail= retry= delay= q=` (after `wire:`, before `ghost:`; the alert fields first, since the `packet:` line is cut at 2040 bytes from its end) | 300 render frames | `bad`, `bare`, `over`; `young owner` whenever `dmgdelay.on` is armed | `owner=` hits applied/refused as the victim's owner; `by=` a bystander's applied as the same unit / applied undecidable / refused as stale; `slots=` the table's count (10·N + 1); `B=` the carried messages' bytes against `stockB=`; `bound=` copies the `0x2C` made with a real lower bound, `unk=` those without; `held=` creates the hold moved, `fallback=` those it gave a slot freed in the last tick, `holdfail=` those it failed, `retry=` Deathmatch respawns that found no slot and wait for their countdown's next fire, about 180 ticks |
| `… \| ghost:[ LEVER-OFF] over= bad= offthread= unbound= short= q= deep= replay= now= killed= swept= nokill= untracked= inactive= stale= cleared= levels= pos ground= air= none= off=` (after `hits:`, the line's last section; the alert fields and the lever first) | 300 render frames | `over`, `bad`, `offthread`, `unbound`, `short` | `q=` creates held during the load and `replay=` those replayed before the first tick (a `replayed slot …` line each); `now=` creates made in a catch-up tick (a `created slot … in a catch-up tick` line); `killed=` held creates a refused `0x0C` cancelled, `swept=` copies made before state 6 that a kill refused in the catch-up ticks marked dying; `pos ground=`/`air=` dirty creates placed from their move payload, `none=` left at stock's record, `off=` a payload off the map, `unbound=` a reader that is not B3's (B5 refuses to install without B3 armed), `short=` a position read that would pass its message's end; ` LEVER-OFF` with `ghostq.off` |
| `kills: bad= bare= late= full= noslot= b12= out= in= st5= sw= same= fix= rev= dead= cr=F/U/A/I held=` (a line of its own, logged right after each `packet:` line; its length has a compile-time bound, so it is never cut) | 300 render frames | `bad`, `bare`, `late`, `full`, `noslot`, `b12` | the kill counts (B8): `out=` deaths sent carrying the owner's fraction, `in=` taken; `st5=` refused during the load and `sw=` those whose copy B5 marked dying with the owner's fraction; `same`/`fix`/`rev` how a copy's fraction compared at the death; `cr=` copies made finished / unfinished / without a state / with an invalid one; `held=` the creates held; `late=` a hold flushed early by a unit script's send (that copy starts as stock's) |

## Names that do nothing

These appear in older notes and commit messages. No file in the tree reads them; creating one
changes nothing and logs nothing, which is the failure mode to recognise.

`tagpu_fogwide.off`, `tagpu_glshot.trigger` (and `tacli glshot`, which no longer exists), `tagpu_gldbg.on`,
`tagpu_writeback.on`, `tagpu_posedraw.on`, `tagpu_posefix.off`, `tagpu_posewatch.on`,
`tagpu_poserecon.on`, `tagpu_purevk.on`, `tagpu_selgeom.on`, `tagpu_shade.off`,
`tagpu_rglsl.step`, `tagpu_shadowdump.on`, `tagpu_shadow.ab`, `tagpu_unit.on`, `tagpu_unit.ab`,
`tagpu_restore_<tag>.rgba` without `_vk` (the GL half of the restore dump), `tagpu_<pass>_gl.ppm`,
`tagpu_restorevk.on` (the restorer follows Classic++'s `assets=` knob, which the render-options
screen's `Undithered assets` row writes; `classicpp.cfg=assets=0` is the A/B), `tagpu_zoomedge.off`
(there is no camera setting: the centre range holds whenever our terrain pass owns the ground,
the engine's own range whenever it does not, and nothing else chooses between them),
`tagpu_replayhud.on` (the replay HUD draws on every Vulkan replay, with no lever; its scale is
its own button's, kept in `impure.cfg`'s `replayhud=`).
