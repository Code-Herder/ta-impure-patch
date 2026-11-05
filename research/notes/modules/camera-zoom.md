# Camera and zoom

Zoom, its live gate, zoom to the cursor, the widened viewport, the camera's range and the cursor at zoom.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.3 Zoom (`tagpu_zoom.c`, `zoom.on`)

| VA | What it is | Mechanism |
|---|---|---|
| `0x41C426` | `call 0x466B70` — minimap view rect, eye clamped at top | call-site redirect; the engine fills the rect, we rescale it by 1/z |
| `0x41C442` | `call 0x466B70` — ...and at bottom | call-site redirect |
| `0x41CEAA` | the scroll poll `0x41CE90`'s cap on its step: `cmp ebx,0x80; push edi; jle; mov ebx,0x80` (14 bytes, up to `test ebx,ebx` at `0x41CEB8`) | **byte patch** `push edi; call stub; nop×8`, on its own 48-byte match of `0x41CE90..0x41CEBF` at process attach, outside the list the rest of this table is matched by all-or-nothing; the stub (`pushad`, `zoom_scroll_step(ebx)`, the result into pushad's EBX slot, `popad`, `ret`) returns the engine's signed cap divided by the game thread's level, so EBX alone changes |
| `0x41C3C0` | the eye clamp — `eye = clamp(eye, 0, extent − W)`, `extent` the scroll extent `main+0x1422B`/`+0x1422F` (§2.3c), plus the minimap rect as its last act | **`leaf_call` detour, 5 stolen**, on a flag that is up whenever the module is installed; the replacement clamps to the range in force (§2.3c) and calls the same minimap wrapper |
| `0x41C808`, `0x41C93B` | the inline target clamps of the smooth arms of centre-on-point `0x41C7C0` and centre-on-object `0x41C8E0` | **block replacement**: the block's first instruction (5 bytes) becomes a jump to a stub that clamps the scroll target into the range in force and resumes at the function's tail (`0x41C8BF`, `0x41C9F3`). `SetCamera 0x41C4C0`'s smooth arm (`0x41C4EC`) has the same shape and is **not** patched: no caller reaches it (engine map, row `0x41C4C0`) |
| `0x41CAF7` | the per-frame camera follow's inline target clamp, inside the stepper `0x41CA10` | **block replacement** (6 bytes), resuming at `0x41CB44`; clamps the target into the range in force and clears fog bit 3, as the block's own tail did |
| `0x468DBA` | `call 0x418310` — the map debug overlay | call-site redirect; the overlay runs only for an eye in the engine's own range, because its cell window has no lower bound |
| `0x498EF9` | `call 0x484B50` — the `GetTPosition` inside the mouse → world conversion | call-site redirect; clamps the world point to the scroll extent (§2.3c, "The pointer"), a no-op for any eye the engine's own bounds can produce |

**The camera keeps the view centre on the map at every zoom (§2.3c).** **The level comes from
three levers, and none is an engine patch.** `tagpu_zoom.txt` is the
scripted one and **wins whenever it exists**; a replay's **Follow or Jump** comes next, its level
carried in the frame packet (`follow_level`) and the wheel pinned to it until a notch or a camera
move ends it ([perspective plan, 4c-2 as built](../tadr-port/demo-recorder-perspective-streams.html#4c-2-as-built));
the **mouse wheel** is the player's, and takes over the moment neither is in force. The wheel needs nothing from the engine because the engine
never wanted it: TA's window procedure dispatches only `0x200..0x206` through its jump table
at `0x4B5E3B`, so `WM_MOUSEWHEEL` (`0x20A`) fails the `CMP EAX,6` at `0x4B5E41` and falls to
a bare `DefWindowProcA` tail call at `0x4B5FA8`. Nothing had to be taken away from anyone.

`tagpu_zoom_wheel()` is offered the message at the two of the three engine doors a wheel can
arrive at (`wndproc.c`'s tail for hardware, the shield's `to_game` for injected; the third,
`wndproc.c`'s `WM_NCHITTEST` arm, carries no wheel) and consumes it only over a
live world viewport, and never while an in-game options screen owns input (`main+0x37EBE`
bit 0, the test the click door makes: `ARMOPT.GUI` and the screens under it sit over the world,
so a notch there is the menu's). **A notch it declines goes on to `tagpu_gui_wheel()`**, the
list wheel, which scrolls a list under the pointer (§2.3e *The engine's UI, observed*;
[GUI gadgets](../gui-gadgets.html) §2.4.1). It does one thing on the message thread — it queues the message as one slot of a
single-producer single-consumer ring: its delta, the point it was aimed at and its counter time
(the slot is written, then the head published with an interlocked store; the render thread
reads the head, then the slots, then publishes the tail) — and the level itself still moves in
exactly one place, `tagpu_zoom_read_lever()` on the render thread, which takes the slots in order.
**The notch is BAR's**: each one multiplies the target *distance* `iz = 1/z` by
`max(0.1, 1 − 0.14 n)`, `n` = delta / `WHEEL_DELTA` (Recoil's `ScrollWheelSpeed −20 × 0.007`), so
×1.163 in and ×0.877 out, and the target is clamped to `[1/8, 1/floor]` in `iz` — the floor..8
in `z` (below) — after each message, as the lab does.
**The ease is BAR's too**: a 250 ms tween on the QPC clock from the level currently drawn to the
target, `g = 1 − (1 − f)⁴` applied to `iz`, starting at the notch's own timestamp, so its length
is the same at any frame rate; a notch mid-tween retargets from the drawn level toward the
previous target × the new factor. A target inside `(0.999, 1.001)` **snaps to exactly `1.0f`**,
so 1× stays the byte-identical identity the transform, the minimap rect and the scroll step all
test for by equality. At the end of a gesture, once the tween has landed, the log says
`zoom: wheel +120 -> 1.163, landed 251.7 ms after the last notch`. While the file is in force
the wheel is *pinned* to it, which is what makes deleting the file a handover rather than a
jump.

**The floor follows the map.** The lowest level is not a constant: it is `min(0.25, fit)`,
computed on the **game thread** by `floor_compute` in `tagpu_zoom.c`, from the zoom apply at the
top of every in-play draw, before the level is set. Two terms, each a property of the level:

* **fit** — `min(W / spanX, H / spanY)`, W×H the true viewport and the spans the centre range
  the camera moves in (`camera_range` with the centre on: the scroll extent bounded by the tile
  map, §2.3c), so at the fit level the view spans the whole extent. It is the lower end of the
  search, not a predicate.
* **lparam** — the widened rect at the level has all four edges in `[−32768, 32767]`, which is
  what a button `lParam` can carry (`tagpu_vpwide_rect_at`, §2.3b). vpwide's refusal calls the same
  function, so the floor and the refusal are one predicate with two callers.

The search runs on Q16 levels `q / 65536` from `ceil(max(fit, 1/64) × 65536)` up to 16384 (0.25):
the lowest when it passes the lparam test, else the smallest passing one by bisection (the rect only
grows as the level falls, so the test is monotone), else 0.25. So the floor is a multiple of 1/65536,
every float compare against it is exact, and it is never above `TAGPU_ZOOM_FLOOR_HI` (0.25), which
is the floor of every map the screen covers at 0.25×. It **fails closed** to 0.25 where the
viewport (outside 64..16384) or the map cannot be read, and at the level end until the next level's
first apply. It is recomputed only when one of its inputs moved — the true rect, the extent, the
PLOT size — and logged then, naming the term that bound it:
`zoom: floor F (fit G) on WxH, extent WxH, map WxH - bound by fit|lparam|0.25|1/64` (`0.25 (the
rect is not int16 even there)` and `no viewport or map to fit` are the two fail-closed forms). By
the arithmetic, Seven Islands (extent 20448 × 20352) gives 0.0346 at 1024×768 and 0.1030 at
3840×2160, both bound by fit; at 640×480 (a 512 × 416 viewport) a map whose extent is taller
than 26 624 px stops at 1/64, and at 5120×1440 (4992 × 1376) Seven Islands stops at about 0.083,
where the rect's right edge reaches 32 767, bound by lparam [arithmetic from the code's formulas;
not yet read off a `zoom: floor` line in game].

**Every reader takes the floor from one place a thread.** The game thread clamps the record's
level to its own floor before any game-thread reader sees it (`tagpu_zoom_apply`: the rect, the
minimap box, the scroll step, the recorder), so a render-thread floor a frame old cannot reach the
engine. The packet carries it (`zoom_floor`), and the render thread takes it once a frame at the
top of `tagpu_zoom_read_lever` (`tagpu_zoom_floor()`, validated into `[1/64, 0.25]`, 0.25 for a
packet without one), before any pass runs: the wheel clamps at it, the mega notch engages at it
(§2.3m), the reach ladder and the native gather size their slabs with it. When it moves (a level
load, a resize, a map's extent) an engaged mega map's target moves with it along a tween (at once
during a cut's hold), and a
wheel target it leaves below it rests at it (`floor_moved`). **The other two levers**:
`tagpu_zoom.txt` parses a level in `[1/64, 8]` and is drawn no lower than the floor; a Follow's
recorded level below this viewer's floor is drawn at the floor and is still the Follow's level for
"seen". `TAGPU_ZOOM_FLOOR_LO` (1/64, Q16 1024) is the sanity bound on every level *value* — a
packet's, a file's, a recording's, a command's — and these eight guards take it: the gather span (`tagpu_zoom_gather_span`), the scroll step, the minimap
box, the cursor transform's `view()`, vpwide's apply and `tagpu_vpwide_rect_at`, `cmd_valid` and
the sweep's level. `tagpu_zoom_gather_span(v, z, floor)` takes a level below the floor at the
floor's span (never the 1× one, which would cull a visible ring) and caps the span at
`TAGPU_GATHER_SPAN_MAX`, 65 536 world px (`tagpu_terr.h`), whatever floor its caller holds.
**The scroll step is divided by the level, in its register, after the engine's cap.** The scroll
poll `0x41CE90` steps the eye by `ScrollSpeed main+0x1434D × main+0x38A3F` (the 30-Hz ticks since
the scheduler's last call), capped at 0x80, and edge scroll and the arrow keys share that one step
in EBX (engine map, [input](../engine/input.html), row `0x41CE90…0x41D060`). `zoom_scroll_step`
divides the capped step by the game thread's level (`min(a, b)/z = min(a/z, b/z)`, so this equals
scaling the byte and the cap together): the picture scrolls at the 1× screen rate at every level,
with nothing saturating, and a positive step stays in `[1, 0x80 / (1/64)] = [1, 8192]` world px. At
`z == 1.0f`, and for a step of 0, it is the engine's own. **Nothing of ours writes
`main+0x1434D`**, so the options screen, its snapshot and restores, the camera reset and the
save see the player's own value, and nothing has to keep a scaled value out of the registry.
**Gaps**: a player whose registry already holds a value the old byte scaling doubled keeps it
(the options screen snapshots the byte, which while zoomed was the scaled one, and a `Cancel` or
`UNDO` put that back where the scaling took it as the player's own and the save kept it) — nothing can tell it
from a chosen one, and the slider or SPEEDS' `RESTORE` resets it; and at z > 1 the step rounds
per poll, ±0.5 world px, up to ±4 screen px at 8×.

A notch at the viewport centre moves no eye at all, and **a notch out adds no displacement
wherever it is aimed**: zoom-out pulls straight back from the view centre, as BAR's does
(`CamSpringZoomOutFromMousePos = false`). **A notch in holds the world point under its own
pointer position (§2.3e)**, and that is a camera move, not a transient bias (§3.1). **The view
centre tweens with the same `g` as the distance, and this is the lab's model**
(`tascene-view.html`, Recoil's target pose): a notch in at `a` moves the target centre by
`(a − c)(iz_prev − iz_new)` on the *target* distances, a notch out leaves it, and the drawn
centre is lerped to it. The game's eye is a sum of deltas rather than a pose, so it keeps `R`, the
displacement the tween still owes: at a notch `R = R·(1 − g_posted) + (in ? (a − c)(iz_prev −
iz_new) : 0)` and the tween restarts; every frame posts `R·(g − g_posted)`. So a notch that cuts
a tween neither re-aims nor drops what the cut tween still owed, and where the eye ends depends
only on the notches' points and targets, never on the frame timing. **MEASURED 2026-09-23**
(G20a, 1024x768, the play defaults, `selbox-facings` on Two Continents, eye held at
`(3000,3000)` and released, the level set through the file and the file removed; two other game
instances were running on the GPU at the time):

| gesture | eye after | expected | log |
|---|---|---|---|
| +1 at the centre `(576,384)` from 1×, five times | unchanged | unchanged | `-> 1.163`, landed 250.8 / 256.7 / 262.5 / 265.4 / 266.1 ms after the notch |
| +1 at `(900,600)` from 1× | `(3045,3030)` | `(3045.36, 3030.24)` | `-> 1.163`, 251.7 ms |
| +1 at `(200,100)` from 1× | `(2947,2960)` | `(2947.36, 2960.24)` | `-> 1.163`, 253.1 ms |
| +3 at `(900,600)` from 1× | `(3118,3079)` | `(3117.92, 3078.61)` | `-> 1.572`, 262.6 ms |
| +1 at `(1000,700)` from 2× | `(3030,3022)` | `(3029.68, 3022.12)` | `-> 2.326`, 264.7 ms |
| +2 at `(300,650)` from 0.5× | `(2856,3139)` | `(2856.26, 3138.53)` | `-> 0.676`, 256.5 ms |
| −3 at `(900,600)`, then −3 at `(200,100)`, from 1× | `(3000,3000)` both times | unchanged | `-> 0.675`, then `-> 0.456` |
| +3 at `(900,600)`, then −1 at `(200,100)` inside its tween | `(3118,3078)` | `(3117.92, 3078.61)` — the +3's own displacement, whole | `-> 1.379` |
| −3 at `(900,600)`, then +1 at `(200,100)` inside its tween | `(2922,2942)` | `(2922.01, 2941.09)` — the +1 anchored at `(200,100)` on the −3's target | `-> 0.785` |
| +3 `(900,600)`, −1 `(200,100)`, +2 `(300,650)`, −2 `(1000,700)`, each inside the last's tween | `(3065,3129)`, `(3066,3129)` | `(3065.80, 3128.84)`; the lab `(3066,3129)` | `-> 1.435`; the lab `1.435` |

The world point under the pointer moves by at most 0.47 world px across a notch in — the
integer eye's rounding, which the residual carries into the next frame. Over a gesture of several
groups the carry can leave the eye up to one world px off the model (0.91 on the −3/+1 row).
**The last three rows are the tween's cut**, each group one `tacli keys` batch (the injector
takes one about every 240 ms, inside the 250 ms tween); a diagnostic build with the tween
stretched to 2.5 s, so that every group lands deep inside the one before, ends on the same eyes
and levels. A zoom-out that cuts a zoom-in's tween leaves the zoom-in's displacement whole and
pulls back about the view centre, as BAR's does: the world point that was under `(900,600)` at
the +3's target ends at `(860.2, 573.5)` on screen. The four-group row is the lab's own scripted
sequence (`tascene-view.html`, one `WheelEvent` per notch, 0.1 s and 1 s between groups): lab and
game end on the same eye and level. The landing time is the
first presented frame at or after 250 ms: at the 60 fps cap every figure lies in
`[250, 266.7)`, and **uncapped** (`--maxfps 0`, about 1600 frames a second, one other game
instance still on the GPU) six single notches in landed **250.2–251.0 ms** after the notch. One
notch in and one out lands on `1.020`, not 1.

### 2.3a The live gate — where `s_live` is raised on the Vulkan-only lane  [2026-09-22]

`tagpu_zoom_wheel()` refuses every notch while `s_live` is 0, and `s_live` is raised in exactly
one place, `tagpu_zoom_publish_view()`. **Between the vulkan-only plan's landing 4b-2 and this
one that function had no caller at all**, so the shipped play configuration answered every wheel
notch with *"zoom: wheel ignored — no zoomed world on screen"*: a scroll wheel that did nothing,
with `zoom.on` and `vpwide.on` both armed and both logging `ARMED`.

The GL composite was the only caller and it went with the GL tail (landing 11-3). Its gate there
was `keyOn >= 0` — "the terrain under you is ours **and** the key/fill inversion against TA's own
surface is in force" — and 4b-2 recorded, correctly at the time, that the Vulkan lane could not
run it because `keyOn` needs `f->surface_tex`. What made the note go stale is not 4c: it is
**the clean cut**. TA's own frame is not composited on this lane at all any more — it is captured
as the reference (`tagpu_surf_sync`) and drawn nowhere — so `keyOn`'s question has no answer to
give, and the thing it tested is gone rather than deferred.

**The publish is at the foot of `tagpu_native_frame`'s hand-over**, after the world passes and
the ghost record, and its gate is that function's own preamble rather than a new test: reaching
that line means a packet exists and `pk->in_game` is set, the packet's viewport passed the
64..16384 bound, `tagpu_zoom_predicted_eye` resolved, and at least one world pass was armed —
each of those is an early `return` above it. A menu, a game not yet loaded, a level teardown
(`tagpu_overlay.c` returns through `tagpu_zoom_frame_end`) and a fully disarmed pass therefore
publish nothing, and the claim expires on the next frame that does not reach the line. It cannot
latch.

**The rect is `pk->vp[]`, the TRUE 1x viewport, not the effective one.** That is what gives the
wheel its two halves for free: `in_viewport` fails over the side panel and every dialog, while at
zoom < 1 the outer ring is *inside* the same rect, so a notch out there still works. It is also
the space the message carries — `tagpu_zoom_wheel`'s `lparam` is game-space, and so is `pk->vp[]`;
the Vulkan composite's own rect (`TAGPU_WORLDTGT.vx/vw`) is the same region in frame pixels and
must not be used here.

**Measured 2026-09-22**, `renderer=vulkan`, 1024x768, `scenario load pose-inventory --defaults`,
Two Continents, camera released, viewport `(128,32 896x704)`:

| | before | after |
|---|---|---|
| `tacli wheel -6 --at 576 384` | 244 px change, refusal logged | `zoom: wheel -720 -> 0.564`, **630 184** px of the 630 784-px viewport changed |
| the command record | `live=0` every frame | `z=0.513 live=1`, `addr=(-298,-303,1448,1069)` beside `vp=(128,32,896,704)` |
| six notches out, six back, at the centre | — | exactly `1.000`, eye unchanged at `(1716,806)`, 372 px of frame noise |
| six out at `(300,200)`, off centre | — | eye `(1716,806) -> (1929,948)`; the closed form `eye + (p−c)(1 − 1/z)` predicts `(1928.95, 947.97)` |
| and six back at the same point | — | exactly `(1716,806)` and `1.000` |
| a notch at `(60,400)`, over the side panel | — | *"wheel ignored — pointer is off the world viewport"* |
| a notch at `(140,40)`, the ring's corner at z < 1 | — | accepted, `-120 -> 0.513` |
| a notch at `MAINMENU.GUI` | — | *"wheel ignored — no zoomed world on screen"* |

**An in-game menu declines it too** [MEASURED 2026-09-26]: with `ARMOPT.GUI` raised over a
zoomed world, a notch over the world logs *"wheel ignored — an in-game menu owns input"* and the
level does not move; once the menu is closed the same notch zooms again.

**What the dead gate was also holding down**, all of it restored by the one line, because each
reads `live` off the command record `tagpu_zoom_frame_end` posts:

* **`vpwide`** never widened the addressable rect — `tagpu_vpwide_apply` takes the zoom apply's
  level (the record's, clamped to the floor) only when the record is `live`, and 1.0 otherwise — so every click and band box in the outer ring at zoom < 1 was dropped, the
  feature §2.3b exists for. Verified after: at `z = 0.513`, clicks on `armsolar` and `armrad`
  (1x screen x = 1110 and 1370, both off the window) select 1 unit each, the gap between them
  selects 0, and a band box over the ring takes the 2 units inside it.
* **The minimap view rectangle** stayed the 1x box. After: 8x7 px at 1.0, 14x13 px at 0.513.
* **The camera range** stayed the engine's at every zoom (§2.3c). After, edge-scrolled to the
  right edge: `eyeX` clamps at **9824** at 1.0 and **10063** at 2.144 — the closed form
  `9824 + (W/2)(1 − 1/z)` gives 10063.00.
* **`ScrollSpeed`** was never scaled, so scrolling at 0.25x crawled.

The **fog** is the one item that never depended on it: the packet carries the whole map's fog
source, which no eye or level can outrun (§2.3, "The fog needs no bound").

**Past the map's edge.** The camera's range keeps only the view **centre** on the map (§2.3c), so
at any zoom the view can reach up to half its own width past an edge; off-map there is the
swapchain clear, which is black (`tagpu_vk.c`'s `s_clear`; the owner ruled on 2026-09-23 that what
a player sees past the map's edge is black until the mirrored edge lands, G20b).

### 2.3a-bis The arm state is published in one store, never transiently zero  [2026-09-10]

The arm block inside **`tagpu_native_frame()`** (there is no `tagpu_native_armed()`; the other
passes have one, this one does not) re-reads its lever every 30 frames. It used to do that by setting
`s_armed = 0`, performing a **file read**, and setting it back — and `tagpu_native_owns_unit()`
opens `if (s_armed != 1) return 0;` and is called **from the game thread** by `tagpu_markown.c`'s
`mark_selbox`. So twice a second, for the length of that read, every selected unit read as "not
ours" and the engine drew its own selection rects into the key-filled viewport, where the GUI
mirror published the boxes and painted them cyan over the world (`gui-renderer.md` §20).

It is now computed into locals and published in **one store**, and `s_armed` is **`volatile`**
(as `s_selComplete` already was) so that store cannot be hoisted above the state it publishes.
That closes the periodic window — the one the shipped configuration hits twice a second.

**Residual, stated rather than papered over:** `s_type` is written only when it has actually
changed, and the pass is disarmed across that write, which **narrows** a window rather than
removing one — nothing waits for the game thread to observe the disarm. It is reachable only
while a human is editing the arm file. Closing it properly wants the type published by index into
a double buffer, which is its own piece of work.

**And one predicate carries the whole ownership decision [2026-09-10].**
`tagpu_native_owns_unit` now also refuses a unit whose **ModelId will not resolve**, because that
decision has four consumers that must agree: the gather skips what it refuses, `tagpu_overlay.c`
leaves the engine's composite unwiped, `tagpu_mark.c` leaves the bar on the engine's anchor, and
`tagpu_markown.c` leaves the engine's own selection rect alone. Wrong in one direction a unit is
drawn twice; wrong in the other it is invisible, or keeps its sprite and silently loses its
selection box for ever — which is what a landing review caught here, against a claim that a unit
with no model was "owed nothing" (`ui-markers.md` §1 carries the correction and the disassembly).

**The general rule this is an instance of:** anything the game thread reads to decide whether we
own a draw must never have a "not yet" state that the render thread publishes on its way to an
answer. A poll that tears is a poll that hands the engine back the frame.

### 2.3m The mega map binding (`tagpu_zoom.c`, `wheel=` in `impure.cfg`)

**The mega map is a state of the wheel, not a second view.** A notch back on the mega map's wheel
**engages** it: the target level becomes the floor (`tagpu_zoom_floor()`, this frame's floor and
the lowest level the wheel can reach, §2.3 *The floor follows the map*; while engaged the target
follows the floor when it moves), the view centre moves onto the map's centre, and the target it
left is kept (`s_megaBack`). While it is engaged, **any notch forward leaves it** — TADR's gesture — back to the
kept target, holding the world point under the pointer (or centring it, below). A mega notch with
nothing to do (back while engaged or already at the floor, forward while not) moves nothing, as
TADR's does — engaging at the floor would keep the floor as the level to return to, so the eye would
move to the map's centre and the next notch forward would bring no level back; a zoom notch
back while engaged is already at the floor and leaves it engaged. The design: [mega map plan](../mega-map-plan.html); the strategic
icons the zoomed-out levels show, and their pick: [strategic icons](icons.html).

**Which wheel is the mega map's** is decided on the message thread, per notch, by
`wheel_is_mega`: Ctrl picks the action the plain wheel does not do, and `wheel=` says which that
is — `zoom` (the default: the wheel zooms, Ctrl+wheel is the mega map) or `megamap` (TADR's
binding). Both doors carry the modifier keys in `wparam`'s low word (the hardware message in
`wndproc.c`, the injected one built in `tagpu_shield.c`), so the slot records the answer
(`NOTCH.mega`) and the render thread never reads the store. The row is *Mega map wheel*
(Ctrl+wheel / Wheel) in both panels ([settings](settings.html) §2.12); under `tagpu_defaults.off`
the store has no say and the wheel zooms.

**Engaging shows the whole map from wherever it is engaged.** The floor is the level at which
the centre range's rectangle fits the viewport (§2.3), but the range keeps only the view centre on
the map (§2.3c), so a pull-back about the view centre from a corner would leave most of the map off
screen. Engaging therefore also moves the eye to the middle of the centre range (`mega_eye`):
the view centre on the middle of `[0, min(extent, tile map)]` on each axis — the same rectangle the
floor's `fit` is computed from, taken from the packet's copies of the floor's own inputs (`vp`,
`map_pxw/h`, `map_w16/h16`), so the render thread reads nothing new. The displacement is the
target eye less the eye the gesture lands at as of that notch — this frame's eye (the packet's plus
what is posted and not yet acknowledged), the carry, what the tween being cut still owes, and the
terms of the frame's earlier notches (`mega_centre_shift`) — and it is paid through `R` like a
notch's term, so the cut below posts it with the level and `anchor_step` clamps it per axis. With
no packet in play or no range to take the centre from, the
level still falls, about the view centre. A map whose scroll extent is narrower than the viewport
at 1× is the one case the first frames' clamp can cut: while the packet still carries the engine's
range (`cam_centre` 0, a draw whose ground is not ours) its `[0, extent − W]` holds the eye at 0, and an
axis the clamp cuts drops what it owes (§2.3e).

**Leaving is the zoom-in anchor by default.** Any notch in holds the point under the pointer by
moving the target centre `(a − c)(iz_prev − iz_new)`, and leaving is such a notch from the floor to
the kept level, through the same owed displacement `R`, so the point stays under the pointer at any
frame rate, away from the clamp. A map edge's range can cut the step per axis, as for any notch.
`megazoom=tadr` (the *Mega map zoom in* row, stage *TADR default*) moves the target centre onto the
point instead, `(a − c) · iz_prev`, so its new view is not inside the old one; the step is up to half
a viewport times `1/floor`. The notch's slot carries the setting (`NOTCH.tadr`), as it does the
binding, so the render thread never reads the store.

**Entering and leaving are a cut: the frame before is the old view and the next is the new one, with
nothing drawn between** (`s_cutHold`). The wheel's 250 ms tween is for the zoom; these two skip it.
A cut could not simply draw its target at once, because the game thread answers a frame or two
later: the ladder would hold the level at a rung and the reach bound would clamp the eye short of
a step this long, an in-between view for those frames. So the notch's frame sets the target and
posts the whole of `R` (`g = 1`), and that frame's command record carries the eye step, the new
level (`rec.zoom`, the target rather than the level drawn) and its reach rung together, which the
game thread takes in one apply. `predict` keeps drawing the last frame's level and eye, through
the reach bound, and counting `paused=`, against packets that still describe them, until a packet
acknowledges the whole step and the ladder no longer raises the level; that frame draws the new
view. The release also fires 250 ms after the notch (`CUT_HOLD_US`), or at once on a view the
ladder no longer allows or with no drawn view to hold; that deadline is not a safety argument,
since the ladder and the bound hold on every frame after it, and it only keeps a stalled game
thread from freezing the picture, at the cost of the in-between view. The log line's `landed` is
the notch to the frame of the cut.

**A spin is one gesture.** A wheel sends many notches, so what follows a cut matters as much as the
cut. The same-way notches behind it, with no pause over 200 ms (`SPIN_GAP_US`, the gesture's feel
and nothing else), are dropped, so a spin out of the mega map lands on the kept level and not past
it. A mega notch with nothing to do is dropped too, so it starts no tween. A batch that does reach
the wheel during a hold joins the cut, posted whole, and does not extend the deadline; a tween
started there would run from the held view, the level the cut is leaving. A reversal is a new
gesture: a mega notch cuts and a plain notch zooms, joining the cut if it lands in the hold, so a
plain forward spin out of the mega map followed by a plain notch back is the plain wheel's tween.

**A mouse record is handled through one view.** A button's point is mapped at the door through a
level, and the engine adds its own eye when it handles the record, and the left button's release
decides the click or the box from the world points at the press and the release
([input](../engine/input.html)). The door, the ring's pop and the apply all run on the game thread:
the main loop dispatches every queued message, IdleTick pops and handles one record, and the frame's
apply follows ([sim](../engine/sim.html)). So a record can meet another pair than it was mapped with
in three ways, each closed by an ordering and none by a time:

- **It is still in the ring when the cut applies.** The record naming a new cut (`cut_gen`) waits at
  `tagpu_zoom_cut_gate` until the ring is empty; the draw takes the last record again, whose fields
  are absolute, so the cut's `cmd_seq` stays unacknowledged. IdleTick pops one record each time it
  runs, so the wait ends, and the message-box loop that runs the pump without that pop runs no apply.
- **A press arrives between the apply and the picture.** `s_cutOwed` stands from before the cut's
  record is posted until a frame of the new view has gone to present (`tagpu_zoom_presented`, so a
  frame that did not present does not count), and the door drops a press on the world with its
  release. The flag falls after `s_zoom` is written, and a deadline release leaves it up until the
  step is acknowledged. **What is left is the display's own latency:** a frame handed to present and
  not yet scanned out, a vblank or two [INFERRED], which no ordering here can see. A press on a panel, the minimap or a GUI screen is mapped 1:1 and
  passes. A frame that publishes no view clears `s_live`, which opens the door.
- **A button is already down on the world.** A mega notch with a world button held is dropped, and the
  map stays where it is. The notch is stamped with `s_worldHeld` when it is taken, in message order on
  the game thread, so a button let go and not yet dispatched still counts. A press after the notch is
  the two-flag case: `cut_may_start` raises `s_cutOwed` and reads `s_worldHeld`, and the door raises
  `s_worldHeld` and reads `s_cutOwed`, each through a full fence, so at least one sees the other. A
  release lost outside the window clears at the apply after one that read every button up: that
  state and the queue change together [INFERRED], and the pump runs between two applies.

While `s_cutOwed` stands, what still reads the door — hover, a move — is mapped through the game
thread's own level (`s_gLevel`), which changes with its eye.

**A recorded cut is a cut.** The apply that takes a cut latches it, and the next recorded camera
sample carries `REC_POV_CUT` (format 28), which `persp_sample` never blends into: a Follow holds the
earlier sample until the tick the sample is due and then shows it. The level and eye the sample
reads changed in one apply. The picture is one frame because a Follow's packet carries the tables
for its own level: the apply takes the lower of the record's rung and the Follow level's rung into
`s_gReach`, the eye and the level are in that packet, and a Follow's frame drops the rungs in flight
above its level. The game thread keeps that rung as a cap after the Follow ends, until a record from
a frame that holds a packet past that Follow and is not following (`follow_free`), so those rungs
cannot raise a later packet's reach and a Follow's end pops nothing. A recording of format 26 or 27 has no such samples and blends as it
always did.

MEASURED 2026-11-04 (Two Continents, floor 1/18, a battle of 401 units, 1024×768 on a private
display, 60 fps video, stock TA, injected input; the click cases with temporary log lines since
removed). Live: each of 22 cuts changes exactly one video frame. A press behind a leaving notch was
dropped, a press on the side panel behind an engaging one was not, and a Ctrl+wheel back with `ddown`
held did not cut. Forcing the gate to defer five draws of each of 6 cuts left each one frame. Clicks
right before a notch never found the ring busy at the apply under injection, so a busy ring was
exercised only by that forcing, and the path that clears a lost release needs real buttons and was
not run. Leaving at five pointer positions from the floor to 1×: the world point under the pointer
differs by 0 px at four, and by 148 px on x at the fifth, where the eye stands at the range's end
(10272). Under `megazoom=tadr`, set by clicking the row in a running game, that point is exactly the
view's centre at three pointers away from the range's end. Follow of the recording: 22 cuts, 22 flagged samples, each one frame; a control build
without the flag's test and the Follow's rung blended cuts over 3 to 5 changed frames (`f44`: 4 over
9 frames, `f606`: 3 over 6).

**Nothing else can leave a stale "engaged".** The file lever, a replay's Follow and the
mouse-world pin each set the level through `wheel_rest`, which disengages, so a later notch forward
can never send the camera back to a level from before them.

**MEASURED 2026-11-02** (Two Continents, 1024×768, viewport `(128,32 896×704)`, the play arm set,
on a build whose floor was a fixed 0.25 and whose engage left the eye where it was; the floor that
follows the map puts Two Continents' at about 0.056 there [arithmetic], so the engaged level now
reads that, and the engaged eye is the map's centre, `(10720/2 − 448, 12672/2 − 352)` =
`(4912, 5984)` [arithmetic, not yet read off the game], and leaving centred the point, now
`megazoom=tadr` — the rows below are that build's):

| gesture | level | eye |
|---|---|---|
| two plain notches back at `(900,600)`, from 1× | 0.769 | `(2376,726)` |
| Ctrl+wheel back at `(900,600)` | **0.250** (a plain notch gives 0.674) | `(2376,726)`, unmoved |
| Ctrl+wheel back again | 0.250 | unmoved |
| plain notch forward at `(300,200)` | **0.769**, the kept target | **`(1272,−10)`** = `(2376,726) + (−276,−184) × 4` |
| plain notch forward at the centre, not engaged | 0.895 (×1.163) | unmoved |
| Ctrl+wheel forward, not engaged | 0.895 | unmoved |

The same sequence under `--defaults` with `wheel=megamap` gives the same levels and eyes with the
roles swapped, and a click on the in-game row changes the next notch's meaning without a relaunch.

### 2.3e Zoom to the cursor (`tagpu_zoom.c`, no arm file)

**Only a notch IN anchors, each at its own point, and the displacement is paid along the tween
(§2.3, G20a).** `wheel_level()` takes each notch's term `(a − c)(iz_prev − iz_new)` on the target
distances into `R`, and `anchor_step()` posts `R·(g − g_posted)` a frame, pre-clamped per axis (an
axis the clamp cuts drops what it still owes). A notch out adds nothing and pulls straight back
from the view centre, as BAR's camera does. The derivation below — `d` across a change in `z` —
is the same arithmetic taken per notch rather than per frame. The measurements below that zoom
out at an off-centre point (the "zoom out" row, the −4 gestures) are the 2026-09-10 rule's; §2.3
carries the current figures.

**[CORRECTED 2026-09-12, frame packet landing 2 — §2.17 is the current mechanism.]** The rule,
the arithmetic and the three properties below are unchanged and were re-measured on the landing
(the same four gestures, exact on every axis; the hover oracle; the follow release). What
changed is WHERE the eye is written: the render thread no longer touches `main+0x1431F` or any
other engine word. It works out the step in `tagpu_zoom_read_lever()`, adds it to a cumulative
sum that rides the command record, and draws this frame from the packet's eye plus every delta
the game thread has not acknowledged; the game thread applies the difference at the top of its
next in-play draw, releases the follow there when the gesture asked, clamps, recomputes the
minimap box, sets its dirty bit and clears the fog flag itself (§2.3, "The minimap's dirty bit"). So the paragraphs below about *render-thread*
stores, the `s_dropFollow` request that `terr_fogtick` consumed, the residual "kept whole while
it waits", the fog request/ack pair and the gate on terrown owning the fog draw describe the
2026-09-10 mechanism and are kept as its record. The "named gaps" paragraph's read-modify-write
on an unaligned field racing the stepper is closed: one thread writes the eye now.

The wheel holds the world point under the **pointer** still, instead of the one at the centre
of the screen. No engine patch and no lever: the transform is a similarity about the viewport
centre and nothing in it is free, so the one variable that can hold a point is the engine's
eye. With `W(s) = eye + vw/2 + (s − c)/z`, holding `W(a)` across a change in `z` is

    d = (a − c) · (1/z_prev − 1/z_now)

added to the eye, where `a` is the point the notch was aimed at. Applied on the render thread
inside `tagpu_zoom_read_lever()`, which is the call the pass makes before it reads the eye, so
the zoom and the eye it is drawn with change together.

**Fields we write:** `main+0x1431F`/`+0x14323` (the eye) and `main+0x14327`/`+0x1432B` (its
scroll target) — always together, because the two disagreeing is what the per-frame stepper
reads as a camera move in flight and would rebuild the fog grid every frame for as long as it
lasted (G13g) — `main+0x142CB`, the minimap's view box, recomputed through the engine's own
`0x466B70` because `0x41C3C0` is the only place it is otherwise filled — after an eye it moved,
and also, while the redirects are installed, when the level differs from the one the box was last scaled by (the box is a function of
both: MEASURED 2026-10-07, a paused replay kept the 1x box through wheel steps to 0.456 and 0.25
until the eye moved) — and, on a frame that
actually steps the eye, the three **camera-follow** slots zeroed (below). After any eye it moved
the apply also writes the two flag bits the engine's own writers do, on the game thread: it sets
`main+0x142F1` bit 1 (the minimap's dirty flag) and clears `main+0x14281` bit 3 (the screen fog
grid's is-current flag) — the rows on both in [exe reverse engineering](../exe-reverse-engineering.html)
say why only that thread may.

**A frame that wants to step the eye asks the GAME THREAD to release the camera follow**
[G13u, 2026-09-10]. Following a unit, the stepper `0x41CA10` recomputes the scroll target from it
*every* frame and clamps it inline, so the anchor's delta was eased straight back out and the zoom
read as pinned to the unit. The engine answers the same problem in its own edge and hotkey scroll
by releasing the follow — the scroll poll's eye-writing tail clears `main+0x1434B`, `+0x142F3` and
`+0x142F7` at `0x41D091`…`0x41D0AA`, the body of `0x41C390`, and `0x41D035` skips that tail on a
frame where the eye did not change.

**Those three stores are ours to make only on the game thread, and that is the whole safety
argument** — the same handshake as the fog request above, for the same reason plus one more.
`main` is **randomly misaligned at every launch** (`0x41D920` pads it by
`(GetTickCount() % 1000) * 7`; see [exe reverse engineering](../exe-reverse-engineering.html)), so
`main+0x142F7` is 4-aligned in only 25 % of launches, and a dword there is tear-capable in 4.7 % of
them on Intel's documented guarantee (it straddles a cache line) and in **37.5 %** on AMD's (it
straddles an aligned 8-byte boundary; the reference setup is an AMD part — [cross-thread engine
reads](../cross-thread-engine-reads.md) §3). A cross-thread store of zero could therefore be torn — into
a slot the stepper **dereferences** at `0x41CA58`/`0x41CA95`. The eye and the scroll target are misaligned
in exactly the same way and are written from the render thread anyway, because a torn *coordinate*
is bounded by `clamp_pair()`; a torn *pointer* is a wild read.

So: `anchor_step()` publishes a level — `s_dropFollow`, "a gesture is trying to move the camera
right now" — and `tagpu_zoom_follow_tick()` does the stores from `terr_fogtick`, gated on the same
`tagpu_terrown_owns_fog()` that anchoring itself is gated on, so the request always has a consumer.
(That accessor is deleted since 2026-09-23: nothing called it any more.)
Three consequences, all deliberate:

* **The residual is kept WHOLE while it waits.** A frame that finds a follow set returns without
  spending `s_residX`/`s_residY`, so the gesture's total displacement is still exactly
  `(a − c)(1/z_start − 1/z_end)` across the wait rather than losing the frames it spanned.
* **A level, not an edge**, because the engine's own writers are guard-then-store with the compare
  well before the store — 43–46 bytes for the countdown writers, 8 for the two that write the
  object slot (`0x49AE84`→`0x49AE8C`, `0x49C7F3`→`0x49C7FB`): a follow re-established in that
  window would survive a one-shot request, and is taken away again on the next tick.
* **The game thread CONSUMES the request and the render thread re-arms it** every frame it still
  wants the camera. That bounds the one thing a level cannot bound by itself, a producer that
  *stops*: `terrown` keeps skipping for up to 90 frames after the native pass goes quiet, and a
  frozen level would spend those frames deleting every follow the player established. Consumed, a
  dead producer costs exactly one release.
* **A game thread that stops DRAWING never releases, and the anchor then never steps the eye** —
  the camera keeps following, which is the old behaviour and the fail-safe direction. **Pausing the
  sim is not that**: `0x4848E0`'s sole call site `0x469D8E` is inside the per-frame world draw, so
  a paused game still services the request.
* **The banked debt dies with the claim.** A gesture that ends without ever getting the camera
  voids its residual (`drop_claim()`), and so does every exit that gives the claim up. Without that
  the bank survives the gesture — nothing else spends it, since every frame at rest returns
  before stepping — and the next notch anywhere on the map discharges it in one frame as a silent
  camera jump. It is also what kept the centre-aimed control honest: `nx` tests the *accumulated*
  residual, not this frame's contribution, so a banked value would make a centred wheel step and
  release. [All four found by the landing review, 2026-09-10.]

**Also in the fields we write, by consequence:** releasing the follow zeroes `CameraToUnit`, which
is `+0` of the camera block `0x469BFC` hands the order-marker driver — so from that frame an
**unselected** followed unit's order lines stop being drawn (`tagpu_order.c` reads it as `camU`; a
*selected* unit takes the same mask on the next branch and is unaffected, so the Ctrl+C case loses
its lines only while nothing is selected). Same thread, no tear, and it is the state the engine
reaches after its own edge scroll; noted because marker-parity captures depend on it. It applies
under the partial arm set too (`vpwide.on` + `terrown.on` without `zoom.on`), which is a
configuration the module otherwise declines to drive.

**Measured 2026-09-10** on the landing binary, same skirmish, commander followed with Ctrl+C, four
notches aimed at `(900, 600)` against a viewport centre of `(576, 384)` — same gesture and
direction on two builds differing only in whether the request is published, from a start eye within
2 px (the followed commander drifts between runs):

| build | gesture | eye | follow after |
| --- | --- | --- | --- |
| request removed | −4, z 1.0 → 0.683, from `(5127, 7609)` | `(5125, 7609)`, Δ `(−2, 0)` — cancelled | still set |
| as landed | −4, z 1.0 → 0.683, from `(5127, 7611)` | `(4977, 7511)`, Δ `(−150, −100)` | released |
| as landed | +4, z 0.683 → 1.0, from `(5127, 7609)` | `(5277, 7709)`, Δ `(+150, +100)` | released |

Both landed rows are the predicted `(a − c)(1/z_prev − 1/z_now)` = `(±150.4, ±100.3)` to the pixel,
so the game-thread wait costs no accuracy. The control — the same gesture aimed at the viewport
centre — moves the eye by nothing, leaves the follow **intact** and logs no release.

**The leak regression** [2026-09-10, after the review]: three gestures in a row, each re-following
with Ctrl+C first, gave Δ `(−150, −100)`, `(−150, −100)`, `(−151, −100)` against a true
`(−150.37, −100.25)` each. Totals: **X 451 against 451.1, Y 300 against 300.7**. Both sit inside
the ±1 world px that a carry entering and leaving the run can move a three-gesture total by, so
this is the carry working, not a bank — a leaked bank would have shown as a jump of hundreds, not
a pixel. *(Quoting X alone as a telescoping proof would be cherry-picking: Y is the looser of the
two and is the one to check.)* A centre-aimed wheel taken immediately afterwards, with that carry
standing, left the eye at `(5113, 7624)` unchanged and the follow set.

**Three properties the tests lean on.** The delta is exactly 0 with the pointer at the viewport
centre, so that case is the previous behaviour — which is the A/B control, and why this needs no
lever. *Precisely*, since G13u ties the follow release to the eye actually stepping: `nx` tests the
ACCUMULATED residual, not this frame's contribution, so the control holds while the carry standing
from an earlier gesture is under half a world pixel — which `drop_claim()` now guarantees, because
the only residual that survives a gesture is the sub-pixel remainder of a frame that did move the
eye. The one case left is a carry of exactly ±0.5, where `iround`'s half-away-from-zero gives ±1:
a centred wheel would then step one world pixel and take the follow with it. Not reproduced, and
not special-cased — changing the rounding would break the in-then-out oracle's exactness. The steps **telescope**, so total displacement is
`(a − c)(1/z_start − 1/z_end)` whatever the frame timing, giving an exact oracle on the eye.
And in-then-out with a still pointer returns the eye exactly.

**Four gates, each a positive condition.** The wheel only — `tagpu_zoom.txt` keeps the
centre-anchored behaviour, so every zoom fixture stays reproducible. `tagpu_eye.txt` wins, and
says so once a second. A zoomed world must be on screen. And `tagpu_terrown` must own the fog
draw, which is the safety argument rather than tidiness: only there is the grid's rebuild
decision ours to make on the game thread.

**MEASURED 2026-09-10**, 1920×1080, `crowd-static` on Two Continents, viewport `128,32
1792×1016` so `c = (1024,540)`:

| case | anchor | notches | predicted Δeye | measured |
|---|---|---|---|---|
| control | (1024,540) | +6 | (0, 0) | **(0, 0)** |
| upper-left | (400,300) | +6 | (−272, −105) | **(−272, −105)** |
| lower-right | (1700,900) | +6 | (294, 157) | **(294, 157)** |
| zoom out | (400,300) | −6 | (481, 185) | **(481, 185)** |
| at the map corner, eye already (0,0) | (140,44) | +8 | (−472, −265) | **(−472, −265)** — the eye going negative, i.e. §2.3c's widened range composing with the anchor |

Exact on every axis. In-then-out returned `(1728,702) → (1456,597) → (1728,702)`. At a map
edge the range refuses what it must and the residual is dropped rather than banked: parked at
`(0,0)`, six notches out anchored at the bottom-right held eye **and** target at `(0,0)` for
four seconds with no churn.

**The functional oracle is the engine's own hover state**, `main+0x2CBA` (the unit under the
pointer). Pointer parked on an ARMPW drawn at `(664,186)`, +6 notches: **still unit 17 under
the cursor** anchored, and unit **68** under it centre-anchored. That is the feature, read out
of the engine rather than off a screenshot.

**Fog** (on the window builder of the time): `bare=0` on every `fogwide` heartbeat of the run and the replication oracle
`differ=0` (1972 of 1972 cells). A frame-by-frame scan of a lossless 60 fps capture at the
units, on G13s's green-dominance criterion, put the worst single-frame excursion of the
anchored run at **33 396 px against the eye-fixed control's 45 431** — the control cannot have
an eye-induced artifact at all, so the excursions are the ease changing how much world is on
screen, and anchoring adds nothing above that floor. The criterion was checked live before
being trusted: 196 210 green-dominant px at the units against **0** in the fogged corner.

**What the landing review found, and what it settled** (two reviewers, `high`,
2026-09-10). Three real defects, all fixed on the branch: the eye was stepped *before*
`zoom_eye_range()` was consulted, so a frame with no sane engine state left it unclamped **and**
skipped the invalidation; `anchor_step()` used the widened camera range without the
`g_eyeInstalled` guard the public accessor applies, so `vpwide.on` + `terrown.on` without
`zoom.on` — a reachable arm set, since `tagpu_opt.c`'s `needs` steers a default and not a
requirement — could push the eye past a range nothing would walk it home from; and
`tagpu_zoom_fog_pending()` could latch, because `tagpu_terr.c` drops the skip later in the same
frame than `read_lever()` runs, leaving a bump nobody would ever ack. It now asks
`tagpu_terrown_owns_fog()` first: no consumer, no request. (Deleted since 2026-09-23 with no caller left.)

Two claims the review disproved, corrected above and in
[exe reverse engineering](../exe-reverse-engineering.html): the lowest level a single ease step can
cross 1.0 from is **0.5 exactly**, not 0.489, and the engine grid's slack collapses to the bare
32 px at 1×, not to zero. A gesture that reverses is held by the reach bound (below).

**The fog needs no bound: it is the whole map's.** The packet carries the fog source — two bits a
32-px cell for the whole map (`inc/tagpu_fogsrc.h`, [terrain & depth](../terrain-depth.html) §8) —
and every reader expands it as the engine's builder `0x4843C0` would, so every drawn pixel and every
CPU gate (`tagpu_fog_at`) has its fog at any level, from any eye, on or off the map. What a frame
still needs bounded is its gather: the packet's tables only cover a rect.

**The reach bound on the drawn eye.** *The invariant:* every unit, wreck, ghost and feature anchor a
frame gathers lies inside the rect the packet's tables were built over — its **reach**,
`reach_lo/hi` — so a unit inside it carries its pieces, the anchor table holds its rows and the
effects models were culled against it. *The bound:* `predict()` in `tagpu_zoom.c` clamps the eye
the frame is drawn from, per axis, until the slab `[e + (vw − S)/2 − M, e + (vw − S)/2 + S + M]`
(S = `tagpu_zoom_gather_span(vw, z, floor)`, M = `TAGPU_GATHER_MARGIN`, 256) lies inside the
reach (`reach_bound`), at every level, 1× included. It reads only the packet's numbers, so it holds
for any gesture, reversal or lag.

**The reach is a window about the packet's eye** (`table_reach` in `tagpu_packet_pub.c`):
`tagpu_zoom_pub_window` per axis — the slab at the packet's **reach level** (`reach_level`) about
that eye, grown by **the lead**, a quarter of the viewport on each side (`TAGPU_LEAD_DIV`). The
anchor table and the effects models' cull are built over it. The frame is drawn at no level below
the reach level (the reach ladder, below), so S never exceeds the reach's slab and the interval
holds every eye within the lead of the packet's: the clamp is the identity for a gesture that only
zooms in and for an unapplied displacement inside the lead, and what it holds back is a larger one,
for as long as that lasts. At z ≥ 1 the slab is the 1× one and the reach at least the 0.25× one,
so the clamp could bind only on a step of 1.75 viewports.

**The reach follows the level, through a ladder and a ring** (`tagpu_zoom.c`, "the reach ladder").
A reach taken at the floor would be the whole map at every level on a large map — every unit's
pieces copied and the whole map's anchors scanned on every draw at 1× — so the window is taken at
a level the render thread asks for instead. Every command record carries **`reach_zoom`**: the
rung of the ladder (0.25, 0.125, 0.0625, 0.03125, 1/64, `tagpu_zoom_rung`, clamped to
`[floor, 0.25]`) at or below the lowest level the view may draw before its next record is applied
— the level drawn, the wheel's target (so a notch out asks for its target's rung on the frame it
arrives) and the level the levers named. `cmd_valid` refuses one outside `[1/64, 0.25]`. The game
thread takes the reach at it, clamped to its own floor and to 0.25, and at the floor when there
is no record (`s_gReach`, published as `p->reach_level` beside `reach_lo/hi`). The ladder rather
than the level itself because the publisher's anchor scan is cached per rect within a tick
(`s_aRect`/`s_aTick`): a continuous level would move the rect on every draw of a gesture and
rescan per draw instead of per tick. *The invariant:* every packet the game thread can still
publish carries tables taken at a rung of a request the render thread posted — the current
packet's `reach_level`, or one of the requests still **in flight** (posted, and not yet reached by a
packet's `cmd_ack_seq`) — and the frame is drawn at no level below the floor, the packet's reach
level or any rung in flight (`reach_ladder`). So the slab at the drawn level lies inside the reach
of every packet that can arrive, and a newer packet never asks for a higher level than the one last
drawn: a zoom-out waits at a rung for the game thread instead of popping back, counted as
**`reachheld=`** on the `native:` line. The ring holds eight requests; a full ring merges its two
oldest into one with the later seq and the larger rung, which only holds the level higher for
longer. **A new epoch does not empty the ring**: the game thread applies a record's reach whatever
its epoch (the epoch gates only the eye delta), so a request of the old level still decides a new
packet's reach, and it leaves the ring only when `cmd_ack_seq` reaches it. The interval is empty only for a packet whose reach
was not taken for its own viewport; the slab is then centred on the reach and the frame counted
(`out=`). The units carried with their pieces are those whose projected anchor (`wy − wz/2`, the
pass's own test, so no bound on altitude is needed) lies in the reach (`TAGPU_PK_U_INRECT`); one
carried outside it that the native pass finds inside its slab is not drawn and is counted,
**`nopieces=`** on the `native:` line — only a frame the bound could only centre (`out=`) can make
one. The feature anchors cover the reach plus 16 cells, scanned in whole rows
outward from `mid`, the centre row of the view about the packet's eye clamped into the rect, in
the order `mid, mid+1, mid−1, mid+2, …`: the scan stops at the first row
that does not fit in the room the table has left, and that row and every later one in the order
are dropped, the row at the same distance on the other side among them. An overflow of the
65 536-entry table therefore keeps a band `[mid − a, mid + b]`, `b − a ∈ {0, 1}`, and says so once
per scan (`trunc=` on the `feat:` line). A frame is drawn from the packet's eye plus the steps the
game thread has not applied yet, as far as the bound lets them through — at most the lead
(`vh/4`, 33 rows at 4K). A frame loses nothing while the band holds its own slab at that offset
from `mid`: 560 rows at 0.25× at 4K, the whole map at a floor that fits it. The scan is reused within
a tick only when the tick, the rect and `mid` all match. The densest stock map, Canal Crossing, puts 12 633 anchors in one rect at 4K
with the lead — every feature it has, 19 % of the table; next are Cavedog Links CC at 10 289 and
Plains and Passes at 9 603, each also whole; at 1080p Canal Crossing's worst rect holds 9 740
(the feature cells of the 275 stock maps' TNT files, every placement of the rect, 2026-09-24).
Wrecks made in play add to those counts. MEASURED 2026-09-24 on `feat-forest` at 1920×1080 on Xvfb,
the eye held at three places at 1×, 0.5× and 0.25×, against a diagnostic build whose table held
1 500: where the rect held more (1 494 and 1 499 kept of rects 393 rows tall), the scan kept rows
42..413 about centre row 219 and 117..505 about 294 — only the bottom rows, the farthest from the
view — and the frame's `anchors=` read 41, 89, 561 and 0, 38, 598, exactly the committed build's,
with `outside=0` on all 193 heartbeats and `trunc=` counting 1 081 frames.

**The engine grid's build record.** Our renderer does not read the engine's grid, but two things
still need to know when its builder ran: the source's packer, whose `rebuilt` is "the LOS state
moved in this draw", and the fog oracle, whose premise is that the engine's grid was built at the
eye the draw reads. The publisher takes it off LosType bit 3 (`main+0x14281`): only a build sets
it — `0x484904` right after the lazy test `0x4848F2` and the build `0x4848FA` inside the fog
function `0x4848E0`, and `terr_fogtick` right after its own call — every other store to the word
either clears it or writes it back unchanged, and nothing `DrawGameScreen` reaches through direct
calls, branches or jump tables clears it or stores the eye (engine map, "who sets `main+0x14281`
bit 3", with the walk's method and what it leaves out). The bit is read after the command apply,
just before the draw, and again right after it: clear then set is a build inside the draw at the
eye it was drawn at; set before is no build, however the eye has moved since — the camera shake
`0x41C6F0` moves it without clearing the bit; set before and clear after is not expected and drops
the record (**`recodd=`** on the publisher's `fog:` segment, must read 0). A draw the observer does
not track (the screenshot sweep, a nested draw, a draw past the return-stack depth) and a new level
drop the record, and while there is none the apply clears the bit, as every engine eye writer does,
so the draw rebuilds and makes one. MEASURED 2026-09-24 against a diagnostic build (not committed)
that checksummed the engine grid between records and compared each record with `terr_fogtick`'s own
eye, on Two Continents at 1920×1080 on Xvfb, a 72-unit fight in view with the shake on, the
commander followed on a long move and the fog site handed back and retaken: 3 639 records, 2 947 of
them at our rebuilds and every one at the eye that rebuild read, 692 from the engine's own fog draw
during the hand-back, and the grid's bytes unchanged across all 28 641 draws between records — no
build went unrecorded. With the sim paused under `ARMOPT`, one `tacli eye` jump made exactly one
record: the apply's clear rebuilds the grid at the new eye.

*The drawn view never moves against the gesture.* The clamp's upper end `reach_hi − (vw + S)/2 −
M` falls as the level falls, so a zoom-out that cuts a zoom-in whose displacement the game thread
has not applied would walk a held eye backward frame by frame and snap it forward when a packet
landed — a sawtooth, one tooth per packet. When the bound would step the eye away from where the
gesture puts it, on either axis, the frame is drawn at the last frame's level instead: the zoom
pauses with the eye, and the eye moves only toward the gesture. The bound itself clamps the eye at
that level, so the invariant is untouched. It is enough because, for one packet, last frame's eye
is inside that level's interval by construction, and a newer packet's interval is non-decreasing in
the eye it was built about (its reach is that eye's slab at the packet's reach level, its size fixed
by the viewport and that level, which is at most the level last drawn: the reach ladder) — so it
fails only when the packet's own eye moved against the gesture (a step of another gesture,
the engine's own camera writers); then the step is taken, because the invariant is the one that
cannot give, and counted as `back`. The rule applies only when the bound fitted the eye into a
non-empty interval (`fit` out of `reach_bound`), and the retry is taken only when it fitted too: a
centred eye is not a step the gesture asked for in either direction, and pausing on it would hold
the last level for as long as the centring lasted. The retry draws at the last frame's level only
while that level is at or above the reach ladder's for this frame: below it the slab could leave
the reach of this packet or of one still to come. The tween restarts from the level actually
drawn, so a notch taken during a pause does not jump.

The native heartbeat carries the witnesses — `fog=WxH out=N held=N/Mpx paused=N back=N
reachheld=N floor=F nopieces=N`: `reachheld` counts the frames whose level the reach ladder raised
(a zoom-out waiting at a rung for the game thread), and `floor` is this frame's floor; `fog` is the source the last frame drew with (`0x0` for a packet that carried none, on which
the native pass draws no unit but the watched player's own and no wreck); `out` counts presented frames whose slab is not inside the
packet's reach, and must read 0; `held` counts frames the bound held back and the largest hold;
`paused` the frames drawn at the last frame's level; `back` the frames that moved against the
gesture anyway, and must read 0; `nopieces` the units inside a frame's slab that the packet carried
outside its reach, and so without their pieces, which were not drawn, and must read 0. `out` and
`nopieces` count only frames that reached the screen: the pass gathers on every iteration of the
render loop, through the lane's bring-up, a swapchain rebuild and a skipped acquire, when nothing
is presented, so each frame's share is held until `tagpu_vk_frame` says it presented that frame
(`tagpu_native_presented`, from `render_vk.c`) and dropped by the next gather otherwise. The
publisher's `fog:` segment reads `fog: WxH/serial recodd=N`.

*Why a bound on the eye for the tables and none for the fog.* The fog source is two bits a cell —
Seven Islands, the largest stock map (640 × 640 cells), is 100 KB, and the packet reserves the
2048 × 2048-cell cap, 1 MiB a slot — so carrying the whole map costs less than the window builder
it replaced did at 4K. The tables are not: the unit pieces, the anchors and the models' cull are
per object and grow with the area they cover, and a bound on the eye costs a few integer
operations per frame and no byte of the packet. The lead covers the one posted step the prediction
runs ahead by.

**The lead — built on the owner's call.** MEASURED 2026-09-24 on Seven Islands with the play set,
36 gestures at NW, SE and mid (10240, 10240), on the window the fog grid then shared with the
tables: at 1080p on the reference GPU, 53 held frames with a largest hold of 509 world px before
the lead and 4 and 61 px with it; on Xvfb 24 and 949 px before, 11 and 474 px with it (33, 11 of
them paused, and 779 px with the apply forced 200 ms behind); at 4K on Xvfb 13 and 2511 px before,
8 and 1578 px with it. On Xvfb llvmpipe draws a handful of frames a second, so each held frame there
spans several posted steps and its hold counts compare only with each other. Every run read `back`
and `out` 0. **Not re-measured since the reach replaced the fog bound, nor since it follows the
level** (the mega-map plan's V1a-1 and 1c's checks): at 0.25× and above the window is the same
`tagpu_zoom_pub_window` at the same level, so the holds there are expected to be unchanged; below
0.25× neither the holds nor `reachheld=` have been measured.

**The source is packed on every tracked in-play draw of a Vulkan session**, whoever owns the
fog site. `terr_fogtick` ticks it at the engine's fog call `0x469D8E` on a draw latched to our
terrain; on any other draw the engine's own `0x4848E0` runs there, and the publisher ticks the
packer in that draw's `after` (`wide_tick`), before the fill copies it, telling it `rebuilt` when
the LOS state moved in the draw: bit 3 clear at the apply (the engine grid was not current as
the draw began; the engine's fog function, run on every in-play draw, then rebuilds), or set at
the apply and clear at the draw's end (a stamp inside the draw, `recodd`). The maps it reads
outlive every in-play draw, and both end in the teardown, the counters first: the local player's
LOS counters are allocated by `0x464700`, called for each active player by `0x464990` at
`0x4919C8` inside the level load `LoadGameData_Main 0x4917D0`, and freed in the teardown cascade
by `0x464A00` (`0x491BA9`); MAPPED is allocated in the map load (`0x483CF6`) and freed at
`0x483E70` by the map-free routine `0x483DD0` (`0x491BB3`), later in the same cascade
([engine map](../exe-reverse-engineering.html) §"The two routines that bracket a level"). The
`after` runs after the load and before the teardown, on the thread that runs the teardown. The
draws without our site are a level's first
ones, until our terrain pass has gathered a frame and the game thread has latched its request
(the rect widens only on the request, `tagpu_zoom_apply`); every draw under `terr.on=off` or
`passive`; every draw of a session without `tagpu_terrown` (`terrown.off`); and every draw on
the GDI lane. On the GDI lane nothing reads the source, so nothing packs it: `wide_tick`
runs only while the session's renderer is Vulkan (`g_ddraw.renderer`, stored on the game thread
at DirectDraw creation before the first draw and moved later only by the Vulkan backend's
hand-over to GDI). That is a property of the session, not a per-frame zoom gate: a producer gated
on the level the render thread publishes would pack one frame late at the start of every zoom-out.
MEASURED 2026-09-24 on `big-battle` at 1920×1080 on Xvfb with `tagpu_gdi.on`, on the window
builder that ran there then: no `fogwide:` line.

**A packet cut at a level's start is grown and filled again, never published cut.** MEASURED
2026-09-24 on `big-battle` at 1920×1080 on Xvfb: a packet published with `truncated = 0xfc6d` — the
font, units, pieces, anchors, shade, particle, light, the fog, fog shade and minimap all cut —
because the scenario's units arrived and the fill outgrew the write slot's committed pages; the
native pass drew that frame without its units and with no fog: a wrong frame, not an empty one.
The exchange now grows the slot and fills it again before publishing, and the fill reports its
whole size (§2.16, the primitive), so no frame is published without the tables it asked for while
the slot can grow. **The fog source and the shade table go down ahead of the world tables**, the
reserve holding them at the design point (`TAGPU_PK_RESERVE`, sized for a 2048 × 2048-cell
source), so an overflow past the reserve cuts what follows them. MEASURED 2026-09-24 with the
grids then carried, on a diagnostic build with a 512 KB frame reserve (not committed,
`big-battle`): every publish past the reserve counted (`trunc=432`), the fog carried on every
frame, and the unit table what was cut; at 640 KB the cut landed on the light layer and the
minimap (`truncated = 0x8800`, 428 publishes) with every world table carried, `viol=0`.

**The minimap's dirty bit is set now, and the box never lagged.** Nine of the engine's eleven eye
writers store the eye, set bit 1 of `main+0x142F1` (`orb $2`, e.g. `0x41C598`, `0x41D04D`), call
`0x41C3C0` — which only clamps the eye and recomputes the view box through `0x466B70` — then copy
the eye into the target and clear bit 3 of `main+0x14281`; the stepper's two (`0x41CB5F`,
`0x41CBCF`) set the bit and clear bit 3 first, then step the eye toward the target, which they
never write. `DrawMinimap 0x466B00` redraws only when bit 1 is set and clears it at `0x466B16`; it
is the bit's only reader (`0x466B06..0x466B16`), and every reader of the word is minimap code
(bit 0 is the dots' blink phase [INFERRED], toggled at `0x4665A9`; bit 2 asks `0x466C20` to
rebuild the fog composite), so all three bits are display state. The radar rebuild `0x466DC0`
sets bit 1 unconditionally — its one exit runs through `0x46742F` — and `0x464F80` calls it for the
viewed record `main+0x2A43` (`0x465072`) on every sim tick, from the tick loop `0x495490` (`0x49555F`). So on a
running game the apply's write decides something only on a frame that ran no tick. The apply now
sets it after any eye it moved, on the game
thread, exactly as the writers do; the reason it once did not — a render-thread read-modify-write
racing `0x466B16` — went away when the apply moved to the game thread. The lag the missing bit
should have produced was never seen: measured 2026-09-10 across an anchored gesture, **51 px of
the 106×126 minimap change**, bounding box screen x 26..42 y 7..16, exactly the union of the old
box (26,7,42,16) and the new one (32,11,42,16); and 2026-09-24 with the sim PAUSED (GameTime
1167 before and after) at `k = 1`, three camera moves through the apply moved the drawn box 52 px
each time on the build before the change and on the build after it alike. The bit's other setters
are `0x466DA4` (the tail of the minimap's fog composite rebuild `0x466C20`, called from `0x465572`
on the viewed player's 30-tick pass of `0x464F80`, after the gate `0x46507D..0x465092`, and from
`0x48191A`), `0x46742F` and
`0x460144`; which of them runs while the sim is stopped we did not establish, so the paused
measurement is not explained, and the bit is set because the engine's writers set it, not because
a lag was seen.

**Named gaps.** The eye is an integer in world px, so the anchor can sit up to `z/2` screen px
from the pointer while a gesture is in flight — 0.5 px at 1×, 4 px at 8×; holding it exactly
would need an off-centre scale centre, which vpwide's addressable rect, the tables' reach and
the ring test all assume away. A smooth centring (`SetCamera`'s smooth arm, centre-on-point,
centre-on-object; `SetCamera`'s smooth arm has no caller) issued after a notch sets the scroll target from its own point, without the
anchor's step, and the stepper eases the eye to it, so the centring wins; a notch that lands
while one is in flight moves the eye and the target together, so the centring's destination
moves with it. The target is clamped into the same range as the eye (§2.3c), so both stop on the
same edge. A followed unit is released instead
(`release_follow()`, the engine's own rule for a manual camera move): a follow recomputes the
target every frame, and without the release it cancels the whole gesture's camera move
(measured 2026-09-10: the eye moved (0, −2) where the anchor asked for (−150, −100)).
`eye[0] += nx` is a read-modify-write on an unaligned field (`main+0x1431F` is an odd offset)
racing the game thread's own stepper, and the landing accepts it: every value that escapes goes
through `clamp_pair()` so nothing can address memory the engine does not own, but a lost update
**discards** a camera move rather than delaying it — a minimap click landing inside the window is
simply gone — and this change raises the write rate from "only when a clamp fires" to every frame
of every gesture. And `s_eyeHold` is polled every 15 frames, so the hold gate is up to
a quarter-second late in both directions.

### 2.3b The addressable viewport at zoom < 1 (`tagpu_vpwide.c`, `vpwide.on`)

**On by default since 2026-09-08 through the play defaults (§2.8); opt-in before that.** Nothing
here writes a byte unless the pass was on at DLL attach — `tagpu_vpwide.on`, or the default with
the zoom on and no `tagpu_vpwide.off` — the true rect verified, *and* a zoomed-out view is live.

| VA | What it is | Mechanism |
|---|---|---|
| `0x468D85` / `0x46964F` / `0x469F95` | `call 0x4C6B10` — the three sites in `DrawGameScreen` that copy the viewport rect into the offscreen surface's clip rect (`+0x1C..+0x28`) | call-site redirect; **clamped to the surface AND to the TRUE viewport rect** (2026-09-09, below), because `0x4C6B10` is a bare four-dword store with no clamping: the surface clamp keeps a wide rect from licensing a write outside the allocation, the viewport clamp keeps one from licensing a write outside the rect our key fill erases |
| `0x499221` | `call 0x498DA0` — the mouse → world / map cell / hovered feature conversion, and the ONE reader that uses L and T as the screen→world **origin** (`world = eye + clamp(pos, L, R) − L`) | call-site redirect; redone with the TRUE origin and the WIDE clamp, a straight pass-through whenever the rect is not ours |
| `0x4B5E5F` / `0x4B5EC0` / `0x4B5F0C` | the three arms of TA's own window procedure's `0x200..0x206` jump table, each unpacking the mouse `lParam` with `AND 0xffff` + `SHR 0x10` | **byte patch** → `MOVSX ECX,CX` + `SAR EAX,0x10`. `LOWORD`/`HIWORD` is zero-extending, so a client x of −20 arrived as 65516 and the event was lost; this is Microsoft's own `GET_X_LPARAM`, and for any position a real mouse can report it is bit-for-bit identical |
| `0x48CE15` | `call 0x48C6A0` (`E8 86 F8 FF FF`) — the unit hit test's one call, inside `GetUnitAtMouse 0x48CD80` | call-site redirect to `tagpu_zoom_hit_guard` (`tagpu_zoom.c`), stdcall `(unit, point)`, `ret 8` like the original; a unit drawn as a strategic icon it answers by the icon's circle, any other it passes to `0x48C6A0` when the unit's narrowing cannot wrap and answers "not hit" otherwise (below). Installed with the rest of the widening half and byte-matched with it, so at every level, 1× included |

**The rect's bound is its four edges, not its width.** `tagpu_vpwide_rect_at` is the rect at a
level for the true viewport: exactly the range the cursor transform `u = (s − c)/z + c` produces
about the true viewport's centre, computed with the same formula, plus one pixel of slack each way.
The button `lParam` carries `u` as two int16s (`MAKELPARAM`, unpacked with `MOVSX` by the patched
arms above), so a rect whose four edges lie in `[−32768, 32767]` is one in which every click is
delivered exactly, and the rect may be up to 65 535 px wide; every engine reader of the rect
compares in 32 bits (`0x48BAE0`, `0x4B6720`, `0x4B67D0`, `0x4C1320`; engine map, *Who reads the
eye*). The apply refuses any other rect (it restores the true one), and the zoom floor's lparam
term is the same function (§2.3, *The floor follows the map*), so the refusal fires only on a level
the floor did not produce. The level the apply widens from is the zoom apply's, already clamped to
the floor on the same thread.

**The unit hit test narrows to int16, so its one call is guarded.** `0x48C6A0(unit, point)` takes
the model's root box from `0x4CB650(MODEL_PTRS[unit+0xA6], &min, &max, 0)`, builds four corners at
`min.y`, rotates each by the unit's turn (`0x4B6CC0`) and forms `x − (eyeX << 16) + c.x`,
`alt + c.y` and `z − (eyeY << 16) − c.z` in 32 bits, keeping bits 16..31 of each (`sar 0x10`,
`movsx`, `0x48C73C..0x48C7A3`); then `0x4C1320` tests the pointer against the quad with two signed
32-bit products an edge (engine map, the `0x48C6A0` row of *Who reads the eye*). Nothing the engine
keeps bounds the position it narrows — aircraft off the map or climbing, carried units, the network
record's raw positions, Teleport ([units](../engine/units.html), *What bounds a unit's position*)
— and the widened rect lets a far unit into HotUnits. A wrapped corner makes a self-intersecting
quad that `0x4C1320` can accept for a pointer elsewhere on the screen: a mis-pick or a wrong hover.
So `tagpu_zoom_hit_guard` runs the engine's test only when (1) the model id `WORD [unit+0xA6]` is
not 0 and is below `UNITINFOCount` (`main+0x1438F`), and `MODEL_PTRS` (`main+0x14377`) and its
entry pass the pointer filter — the bound `model_root` uses. Id 0 is no model: the load's fill loop
starts at 1 (`mov esi,1` at `0x42D69E`, bounded at `0x42D6B6`) over a block `0x4D83B0` does not
clear, so entry 0 is whatever the heap left there, and `GetUnitAtMouse` skips id 0 before its call
(`0x48CE05`) [DISASSEMBLED]; (2) `R`, the ceiling of the largest corner norm of
that root box in px plus 2, is at most 8191; (3) in 64-bit arithmetic, with px the 16.16 value
shifted down 16 as the engine's `sar` does, `|x − eyeX| + R`, `|z − eyeY| + R` and `|alt| + R` are
each at most 32 767. *Why it is a bound by construction* (the comment beside the guard carries the
whole argument): two's-complement sums are exact modulo 2³², so each kept value is the true one
whenever the true sum lies in int32, whatever the intermediates did; condition 3 puts every true
sum inside int16 on all three axes, and with the pointer inside the int16 rect and `R ≤ 8191` each
of `0x4C1320`'s products stays under 1.7 × 10⁹ < 2³¹. "Not hit" is the only answer that names no
wrong unit. `R` is cached per model id in a game-thread table built at a level's first in-play
apply while the `0x48CE15` redirect is in, and stamped with the publisher's level serial
(`tagpu_packet_pub_level_gen`), which moves at every level end whichever thread noticed it — a
teardown seen off the game thread returns before `tagpu_zoom_level_end` runs, so the serial, not
that call, is what retires a table. The guard reads the table only while its stamp is the current
serial (`MODEL_PTRS` is written at the level load, `0x42D6AA`, and nulled only by the teardown,
`0x42DCD8`); any other call is measured on the spot with the same `0x4CB650` call. The zoom floor's
cache is keyed on the same serial. `GetUnitAtMouse` runs on the game thread, the table's. *What it costs*: at 1×
nothing — every HotUnit is on a screen at most 16 384 px wide; below 1× a unit is unpickable (no
hover, no click) only while it is more than about `32 767 − R` px from the eye or flying above about
that, which the widened rect reaches only when the floor is bound by lparam (a super-ultrawide
screen) or for an aircraft tens of thousands of px up. A refusal is counted in the log, at most once
a second: `zoom: hit guard - N unit hit test(s) answered "not hit" so far, whose int16 narrowing
could wrap (the last: model M, R R)`. No run has read that count yet.

**A unit drawn as a strategic icon is answered by its icon, before all of that** (`icon_hit`;
[strategic icons](icons.html), *The pick*). At the game thread's applied level and with the class
table the packet's publisher built on this thread, a unit whose model the icon rule hides is hit
when the point lies within `0.45 × edge / z` world px of the anchor `0x48C6A0` projects —
`((x − (eyeX << 16)) >> 16) + 0x80`, `((z − (eyeY << 16)) >> 16) − ((alt >> 16) >> 1) + 0x20` —
computed in 64 bits, without the corner offsets and without calling `0x48C6A0`, so its int16
narrowing is never met for such a unit. A unit carried — through any chain of carriers the native
pass draws, up to 64 hops — by one whose model is hidden answers "not hit", a nanoframe on a hidden
factory's pad included: the picture draws neither (*What an icon hides* in [strategic
icons](icons.html)). Every other unit — a model, an unhidden nanoframe, a dead one, a type row past
the table, a unit `tagpu_native_owns_unit` refuses — takes the guarded engine test above, so at 1×,
where no model is hidden, the pick is the engine's. Each carrier `+0x86` counts only as a
stride-aligned record below the unit array's slot count (`main+0x14357`, `main+0x14351`), and the
walk stops at one the native pass does not draw. Answered picks are counted at
most once a second: `zoom: icon pick - N hit(s), M miss(es) so far`.

**The clip guard needs BOTH bounds, and the second one is the picture's [MEASURED 2026-09-09].**
Clamping the widened rect to the surface allocation is a memory-safety bound and nothing more: it
still leaves the side panel and the top and bottom strips inside the clip, and **nothing ever
repaints those**. Our key fill covers the true viewport only — that is why every pass takes its
rect from `tagpu_vpwide_true_rect()` and not from the field — and the engine repaints the panel
only on damage it knows about, which a stray world draw is not. So one frame in which an engine
drawer runs with the wide rect leaves marks outside the viewport **for the rest of the session**,
and they accumulate.

That is not hypothetical: it is the bug a 500 v 500 fight with the whole army selected produces at
zoom < 1 (`scenarios/500v500.json`, the fixture written for it). The drawer that reaches them is
the engine's own selection rect `0x46A530`, which `markown` hands the WHOLE set back to whenever
the native pass came up one box short (`ui-markers.md` §1) — and `0x467A50` projects each one at
the **unzoomed** position, which at 0.42× is up to 1.4 screens from where the unit is drawn. Two
independent runs of the fixture: **5768 stray green pixels on the panel before, 31 after** (the 31
is the minimap's own view rect), with five hand-back frames in the "after" run and the engine
surface under the buttons reading 100 % palette index 0.

**The clamp is NOT gated on the surface probe [2026-09-10].** It only ever narrows, so it needs
to know nothing about the allocation — and riding it on `IsBadReadPtr` succeeding would make the
bound conditional on a probe, which is precisely what CLAUDE.md refuses as a safety argument. A
landing review caught it there: one call with an unreadable `self` would have re-licensed the
permanent side-panel marks. The allocation clamp above it still rides the probe, because that one
genuinely needs the surface's width and height.

**Two residuals, named:** `mark.on=nocursor`'s capture window (window B) takes
`tagpu_vpwide_addressable()` and then intersects with the ctx clip, so the narrower clip costs
that **debug lever** the ring markers it exists to show — the shipped configuration never opens
it (G13p). And `0x46A530` indexes `MODEL_PTRS[ModelId]` with no zero test while slot 0 is never
written by the load loop, so an engine box drawn for a ModelId-0 unit bounds itself on
uninitialised memory. That is stock TA's own behaviour, not something this stack introduced, and
restoring it is the correct half of the `owns_unit` fix — but it is worth knowing it is there.

Clamping to the true rect is exactly the bound **stock** TA sets at these three sites — unwidened,
the rect they are handed IS the true one — so it can never clip anything the engine would otherwise
have drawn on screen, and at zoom ≥ 1 or with the widening disarmed it is the identity.

The rect it writes is `main+0x37E27..0x37E33` (L, T, R, B) only — **never W/H at `+0x37E37`/`+0x37E3B`**,
because the eye clamp `0x41C3C0` derives `maxEye = extent − W` from them and a negative `maxEye`
makes it alternate between 0 and a negative eye. The true rect is derived from the **screen
dimensions** at `+0x37E1F`/`+0x37E23` — fields nothing here writes — because `0x497F40` computes
`W = R − L + 1` by *re-reading* L, so a store of ours landing in that window would corrupt W.
Until 2026-09-12 that store came from the render thread, W/H were checked against the derivation
every frame and put back when they disagreed. **Since the frame packet's landing 2 the store is
the game thread's** (`tagpu_vpwide_apply`, at the top of the in-play draw, from the level the
command record carries), and `0x497F40` runs on that same thread at game entry, before any
in-play draw — the two cannot interleave, so the repair is gone and a disagreement is only
COUNTED (`vpwh=` on the packet heartbeat, **0** over the landing's runs: 305 779 in-play draws
across two levels, 1 571 648 under the stress lever). The addressable rect the message thread's
ring test reads is published right after the field is written, so it is exactly what the engine
can name.

**The rect is wide only on a draw whose ground is ours [2026-09-23].** The engine's terrain pass
`0x483FA0` is the one reader of this rect that no clip reaches: it places whole-tile copies
(`0x4CBEF1`, 32 rows of 8 dwords, no clip) from the rect's L/T over the unwidened W/H, so under a
widened, negative origin it writes outside the offscreen. `terrown` normally takes that pass away, which is what made widening safe,
but the two were decided by different threads: the rect by the game thread from the zoom level,
the skip by the render thread when its terrain pass is live. Entering a new skirmish while the
last one's zoom was still commanded (the zoom is not reset at level end) widened the rect on the
first in-play draw, and the render thread took the ground back a few draws later, after the new
map's tile atlas was built. The engine drew the ground in between: a crash at `0x4CBF1D`, or on
another run a garbage pointer read from the memory it had overwritten. Reproduced 1 in 1 on a map
change. Now, at the top of every in-play draw, the game thread reads the render thread's request once
(`tagpu_terrown_request`), `tagpu_vpwide_apply(ta, c, terr)` widens only when it is set and
restores the rect when it is not, and then `tagpu_terrown_latch(terr || tagpu_vpwide_wide())`
writes the byte both `terrown` stubs test. The level end latches again with
`tagpu_vpwide_wide()`. One thread writes both the rect and the latch, and the latch is taken
after the rect is final and is set whenever the rect is still wide (a `restore` that could not
run included), so a wide rect and an engine terrain draw cannot meet. Measured: 3 of 3 map changes at 0.25× clean, each logging
`vpwide: not widening at zoom 0.250 - the engine draws the terrain this draw (#1)`, the draw the
old build widened on.

**What the latch costs, from the landing review.** A change of the request now takes effect at
the next latch, not at the stub's own read. The terrain stub at `0x468DB0` is near the top of
the draw, but the fog stub at `0x469D8E` comes after the whole world draw. The non-in-play
callers of `DrawGameScreen` (`0x495C76` and `0x495E66` in the screenshot routine, `0x4962C2` the
movie recorder) never latch and keep the last in-play value until the level end. So after the
render thread withdraws, the key fill and the fog tick can still run for the rest of that draw.
`tagpu_terrown_filled()` and the structure-shadow gate follow the request, which is already
down, so no such frame is inverted. Handing the site back leaves nothing to void: the engine
grid's build record is read off LosType bit 3 whoever builds it (§2.3), and the fog source is
packed on every tracked draw, at our fog site or by the publisher.

**The cursor is not a reader of this rect and no longer needs to be** — §2.3d is why. Until
G13m it was: the engine drew its sprite wherever `GetCursorPos` reported, so widening what it
could NAME also moved where it DREW, and in the ring a widened `u` lands on the side panel or off
the surface (measured at 0.5× with the pointer at screen (320,400): no cursor at the pointer and
a ghost one on the build panel). That is why the draw poll had its own transform. The cure was to
stop transforming that poll at all.

**The stub takes `g_ddraw.cursor` rather than trusting the engine coordinate it is handed,** and
the widened rect is the reason: at 0.5× the range `[0,128)` is reached both by a ring pointer
(transformed) and by a pointer on the panel (passed through 1:1), and without the true pointer to
settle it a click in the world lands in the minimap's click rect. Since G13m the same sample also
*produces* the engine coordinate — see §2.3d.

### 2.3c The camera's range: the view centre stays on the map (`tagpu_zoom.c`, `zoom.on`, G20a)

**The rule is BAR's centre clamp** (Recoil's `SpringController` clamps the ground point at the
view centre to the map and nothing else; [BAR camera port](../bar-camera-port.html) §1.1). The view
is centred on `eye + W/2` at every zoom, `W` being the **true** viewport, so the range is

```
eye ∈ [−W/2, extent − W/2]         the same at every zoom
```

and a map edge can reach the middle of the screen, zoomed in or out. The engine's own `0x41C3C0`
clamps to `[0, extent − W]`, the range that keeps the viewport's edges on the extent at 1×. The
centre range's far end takes the smaller of the extent and the tile map's own size (the PLOT grid
× 16, the packet's `map_w16/h16`): the debug-level `Edge` command writes the extent as the map's
pixel size less an argument it never clamps (`0x416730`), so a negative one names an extent past
the tile map, past the far end the eye's readers were audited for.

**The map is the scroll extent.** `extent` is `main+0x1422B`/`+0x1422F`: the map's pixel size
`main+0x14223`/`+0x14227` less 32 px wide and 128 px tall, written at the level load
(`0x4833B8..0x4833E0`, `sub 0x20` and `sub 0x80`) and rewritten after that only by the
debug-level `Edge` command [DISASSEMBLED]. On Two Continents: PLOT 672 × 800 (so the tile map is
10752 × 12800, which `main+0x14223`/`+0x14227` also read), extent 10720 × 12672 [MEASURED].
**Stock never shows a pixel past the extent** — its clamp holds the viewport inside it — so the
strip between the extent and the tile map's own size is art no player saw, often left unpainted
by the map's author; the load-time mask makes its right strip unbuildable, and its bottom strip
wherever the ground's projected y lies past the extent (§2.90, "Where the map ends"). The view centre therefore stops at the extent, and the terrain and feature passes fold the
mirrored edge at the same line (`tagpu_fx.h` `tagpu_edge_area`, §2.90). **The pointer's guards
are on the extent too**, the bound they were built on: the 128-px bottom margin is
`GetTPosition`'s search window (engine map, "Engine defects we patch"; "The pointer" below).

The range is computed in one function, `camera_range()`, from two sets of the same inputs: the game
thread's (`zoom_eye_range`: `tagpu_vpwide_true_rect` and the extent) and the render thread's
(`range_pk`: the packet's `vp`, `map_pxw/h` — the extent's two words — and `cam_centre`), so a
step the render thread pre-clamps is accepted by the game thread. The terrain and feature passes
read the same `map_pxw/h` for their fold, so the view centre and the fold stop at the same place
by construction.

**The centre range is in force only while the ground is ours, and that is the bound it rests
on.** The engine's terrain pass `0x483FA0` indexes the tile map from the eye with no bounds
check at either end ([engine map](../exe-reverse-engineering.html), "Two per-cell loops"), so an
eye off `[0, extent − W]` under an engine terrain draw reads outside the tile array. This bounds
the eye the pass reads from and nothing more: over a view larger than the map the pass's window
runs past the tile map from any eye — without the window check a fault in stock and on both
cameras, MEASURED on Lava Run at 1920x1440 in a level's first draws, before the terrain latch is
up — and the window check at `0x484057` bounds it there, drawing the part past the map black
(engine map, "Engine defects we patch": "The terrain pass reads off its tile map"). terrown skips
that function on every draw whose terrain latch is up (`g_terrown_own`, §2.3b), and the packet
publisher sets the latch right after the command apply, from the same request the apply was
handed (`terr || the rect is still wide`). So:

* **the apply** runs at the top of every in-play draw, before the draw's first read of the eye,
  takes the centre range only when the terrain request is up (`s_gCentre = installed && terr`),
  which makes the latch up for that draw, and **clamps the eye and the scroll target into the
  range in force on every in-play draw**. A draw whose ground is the engine's therefore starts
  from an eye in `[0, extent − W]`; a live `terr.on=off` walks the eye home on the next draw.
  Without `zoom.on` (the eye clamp not installed, the range always the engine's) it still clamps
  on **every draw that applied a delta or a hold**: the wheel and its anchor need only the
  mouse→world repair, which `vpwide.on` installs alone, and a delta applied after the engine's
  scroll poll had already clamped the frame would otherwise reach `0x483FA0` past `extent − H`.
* **handing the ground back moves the eye inward, and it stays there** — the by-design cost of
  the bound. Whenever our terrain pass gives the ground back to the engine (its 90-frame watchdog,
  a `key=` change, the passive or over modes, a gather that bails or draws no cell, the pass
  disarmed), the next apply takes the engine's range and walks an eye past `[0, extent − W]` inward
  by up to `W/2` (`H/2`) on that draw. When the ground comes back the eye does not return: the
  range is a clamp, not a memory, so a view that had the map's edge at its centre shows it `W/2` world
  px nearer its own edge (on it, at 1×) until the player scrolls back. The level end does the same.
* **every clamp fails closed.** Where the range in force cannot be computed (a main pointer
  outside the sanity window, a zero viewport or map), the eye clamp, the target-clamp stubs, the
  apply and the level end all clamp to the engine's own `[0, extent − W]` from the engine's own words
  (`engine_range`: `main+0x1422B`/`+0x1422F` less `main+0x37E37`/`+0x37E3B`, the top held at 0),
  and the apply publishes `cam_centre = 0` for that draw, so no path leaves the eye or the target
  unclamped. The follow stub clears fog bit 3 on every pass, as the block it replaces did. **The
  one exception is the pointer's guard** (`zoom_tpos_guard`, "The pointer" below): it clamps to the
  extent, to the map's own pixel size less the level load's margins where the extent is not
  positive, and `y` to `mapH − 129` always; it passes the point through only where there is no
  map (every size ≤ 0) or no sane `main` — `GetTPosition` reads `main` itself at `0x484B61` and
  clamps to the map's size.
* **between in-play draws** every engine camera writer ends in `0x41C3C0` or in one of the three
  target clamps below, and all four use the range the last apply chose, which the latch still
  matches. The level end drops both together and walks the eye into `[0, extent − W]`. The
  screenshot tiler (`0x495A30`, the `MakePoster` command) and the movie recorder draw outside the
  in-play gate and so on exactly that pair.

The packet says which range the apply chose (`cam_centre`), so the render thread's prediction and
the cursor anchor's pre-clamp use the same one.

**Every other engine reader of the eye is bounded for any eye in the centre range at any zoom**,
with `vpwide`'s widened rect below 1× — the audit is the engine map's "Who reads the eye" table.
The one other reader without a lower bound is the map debug overlay `0x418310` (a debug key or
the "Contour" command), and its one call site `0x468DBA` is redirected to run it only for an eye
in the engine's own range. **The far ends put the view centre on the extent's right and bottom
edges, and no reader's bound depends on where they are:** the audit was run for far ends 32 px
(right) and 128 px (down) further out, at the tile map's own edge, a range the present one lies
inside, and each is bounded by its own
compare (the sweep's `PLOT − 1` clips, the fog builder's unsigned tests against the LOS block, the
cull's rect test, the positional sound's LOS test), by the terrain latch (`0x483FA0`'s eye; its
window over a view larger than the map by the engine-defect patch at `0x484057`), or by a
test against the engine's range, which still reads the extent (the debug overlay, the pointer's
guards). The fog's off-range switch (below) is none of these: it tests the engine grid's own
window against the map, `PLOT_C/2 × PLOT_R/2` fog cells, not against the extent.

**The scroll target has three reachable inline clamps, and each is replaced, not chased.** Paths
that set the eye and copy it into the target afterwards (`0x41C574`, the path whose clamp call is
`0x41CDE1`, the scroll `0x41D037`) reach the range through the detour. Three others compute the
target and clamp it inline against `[0, extent − W]`, never calling `0x41C3C0`: the smooth arms of
centre-on-point `0x41C7C0` (block `0x41C808`) and of centre-on-object `0x41C8E0` (block
`0x41C93B`, which centre-on-unit calls), and the per-frame camera follow (`0x41CAF7`). The stepper
eases the eye to the target, so a target cut short there stops the camera short and a unit near
an edge would not be centred. Each block's first instruction becomes a jump to a stub that clamps
the target into the range in force and resumes at the function's tail, so the fog-bit clear and
the unwind are the engine's own bytes, with the same side effects as each exit the block had
(engine map, "the sites `tagpu_zoom.c` patches"). `SetCamera 0x41C4C0` has a smooth arm of the
same shape (`0x41C4EC`) that is left alone because nothing reaches it: all four of its callers
push `smooth = 0`, and its address occurs nowhere in the image as data.

**The minimap box** (`zoom_minimap_rect`) is the engine's box scaled by `1/z` about its centre,
then held on the minimap **so that it can never invert**. The engine places the box by the
*extent*, from the viewport's top-left, and the centre range lets that top-left reach W/2 (H/2)
past the engine's own `extent − W` — so near the far edges the box runs up to half its 1× size
past the minimap, its centre onto the minimap's edge, and at a high zoom the whole shrunken box
lies past. Clamping each edge on one side only then left its top below its bottom — measured
2026-09-24 at Two Continents' SE corner as the range then ended (the eye at `(10304, 12448)`, the
tile map's corner on the view centre, 32 and 128 px past the present one), 1024x768, minimap
`x 10, y 0, 106x126`: top 126 against bottom 125 from 6× on, drawn one row below the minimap's
last — a box the GUI renderer drops (`R ≥ L && B ≥ T`) and the engine's own drawer `0x4BF8C0`
would put on the panel below the minimap. With the clamp below, `114..115 × 125..125` at 6–8×
there, on the minimap's last row. So the **centre** is clamped into
the minimap first and then each edge on **both** sides: the edges leave the rounding in order and
a clamp to one interval keeps them in order. At 1× an eye in the engine's range keeps the engine's
own box.

**The pointer.** `0x498DA0` hands a pointer the world point `eye + clamp(pos, L, R) − L`, and with
an eye past the map a pointer over the void names a point off it; the chain from there is
`GetGridPosPLOT` → NULL → `GetGridPosFeature 0x421E60`, which the always-installed guard there
(§2.6c) answers with "no feature" instead of a fault. The `GetTPosition` call `0x498EF9` that
starts it is redirected and the world point clamped to `[0, extent − 1]` — a no-op for any eye in
the engine's own range — so the hovered cell and feature are real ones. **The extent and not the
map:** `GetTPosition`'s answer lies below `(y & ~15) + 144`, so a point with `y ≤ extent − 1 =
mapH − 129` gets an answer on the map, and the 128-px bottom margin is exactly its search window
(engine map, "Engine defects we patch"). vpwide's replica of `0x498DA0` clamps to the same extent. A
right-click past the edge therefore orders a move to the nearest point of the extent: the
pointer's reach stops where the view centre does, 32 px short of the tile map's right edge and
128 px short of its bottom.

**The fog.** The engine's grid builder places its four border completions on the grid's literal
first and last-but-one rows and columns, which straddle the map edge only while its window
overshoots the map by at most one cell (engine map, "The four border completions"); past the
engine's own range they land off the map. Our renderer never samples that grid: the whole map's
fog source is expanded with its completions on the map's own edge entries, at every eye in the
centre range ([terrain & depth](../terrain-depth.html) §8).

**The scroll poll's right edge at zoom > 1** fires because the poll reads the true pointer
(§2.3d): TA's poll (`0x41CE90`) fires on an **equality on the outermost pixel** — `x == 0`,
`y == 0`, `x == screenW − 1`, `y == screenH − 1` — taken from `[obj+0x196]`, and the screen's right
column is the viewport's right column, so a contracted pointer could never satisfy it.

**MEASURED 2026-10-25, the far ends at the extent** (1024x768, `W = 896`, `H = 704`): on Painted Desert (extent 9216 × 9216; the terrain heartbeat `terr: … map=289x292
area=288x288`) the eye settles at `x = 8768` (at 1×) and `y = 8864` (at 0.5×) against the right
and bottom edges, `extent − W/2` and `extent − H/2`; on Assault on Suburbia (extent 8064 tall) at
`y = 7712` (at 1×).

**MEASURED 2026-09-23** (G20a build, 1024x768, the play defaults, `selbox-facings` on Two
Continents: map 10752 × 12800, extent 10720 × 12672, `W = 896`, `H = 704`). The range's far ends
were then the tile map's (the PLOT grid × 16), so the centre range was `[−448, 10304] × [−352,
12448]`; today's on the same map is `[−448, 10272] × [−352, 12320]` [computed from the extent,
not measured there]. The near ends are unchanged; the far corners' numbers below are the old
range's:

* **The corners.** Held at `(−99999, −99999)` the eye and target read `(−448, −352)` at 0.25×, 1×
  and 8×; held at `(99999, 99999)`, `(10304, 12448)` at all three. In a window capture the map's
  corner sits on the view-centre pixel `(576, 384)` at all three levels: at the NW corner the
  first terrain column and row are 576 and 384, at the SE corner the last are 575 and 383.
* **Edge scroll** (pointer on the edge pixel): a minute per edge per zoom, the eye started 1500 px
  short, settles and stays at `x = −448` left and `y = −352` up at 0.25×, 1× and 8×; 20 s per edge
  from `(9000, 11000)` settles at `x = 10304` right and `y = 12448` down at all three.
* **The wheel at each corner**, −8 notches at `(300,150)`, +16 at `(900,650)` and −8 at
  `(150,700)`: the zoom-outs leave the eye where it is, and the +16 (0.351 → 3.915) moves it only
  along the axes the corner leaves room on, by the notch model's `(a − c)(iz_prev − iz_new)` =
  `(840.4, 690.0)`: NW `(−448,−352)` → `(393,339)`, NE `(10304,−352)` → `(10304,339)`, SW
  `(−448,12448)` → `(393,12448)`, SE unmoved. `tagpu.log` has no fault or violation line:
  `viol=0 pviol=0 crcbad=0`, `relbad=0 woob=0`, `unacked=(0,0)` on every packet heartbeat.
* **The lab stopped at the same place.** `tascene-view.html` opened past the SE corner and nudged
  with the arrow keys read `eye 10304,12448`. The lab's `range()` is now `[−vw/2, (w16·16 − 32)
  − vw/2]` and the same with `h16·16 − 128` — the extent, as the game's.
* **The picture is untouched inside the old range.** The same instance, `selbox-facings`, the eye
  held at the scenario's own camera `(2376,726)`, one pass armed per capture, the base build
  (`78c22b2`) and this one swapped in the gamedir: the terrain pass at 0.5×, 1× and 2× and the unit
  pass at 1× and 2× are **0 px of 3 145 728** apart (`tools/vk-ab.py`), md5-identical; the terrain
  captures carry 2.52 M non-black pixels and the unit captures 8 465 and 22 973. Re-run on the
  final build: the same five md5s.
* **The scenario camera** (`tagpu_scenario.c`'s `place_camera`) runs on the game thread from the
  flip observer, which fires at any of the flip `0x4C63A0`'s 44 call sites behind a 16-ms clock
  gate — inside `DrawGameScreen` at `0x46A3DB`, after that draw's fog site, or elsewhere
  (`0x467E41`, the HUD panel painter's). So it does not write the eye: `tagpu_zoom_place_eye`
  clamps it into the range in force (the last apply's, the one the terrain latch holds; the
  engine's own where it cannot be computed) and hands it to the command apply at the top of the
  next in-play draw, which writes the eye and the scroll target before that draw reads the eye or
  builds the engine's fog grid, then recomputes the minimap box, sets its dirty bit and clears
  bit 3 of `main+0x14281`, whether or not the sim ticks. The first packet carrying the new eye
  therefore carries tables whose reach is about it. Measured 2026-09-24, `camonly.json` (a camera
  line and nothing else) applied at rest after the eye was parked at (6000, 6000): the first frame
  drawn from the new eye (2376, 726) was not held, the fog oracle read `differ=0` from then on and
  the heartbeat's `held` did not move. Written from the flip observer instead, the eye would reach
  a packet built about the old eye, and the bound would draw that frame short of the target — by
  the landing review's arithmetic up to ~1376 px at 1× and a 896-px view.
  Where no apply runs (the packet publisher
  count-only, when nothing of ours draws the world) the eye is written on the spot, the same way.
  A fixture that asks for a view past the engine's range frames where it asked while our terrain
  pass owns the ground.
* **Without `zoom.on`, what the apply applied is clamped.** `vpwide.on`, `native.on=all` and
  `mark.on`, no `zoom.on` and no `terr.on`, `selbox-facings`, and a diagnostic build that logs every
  applied delta leaving the engine's range. 20 rounds: the eye held at `(3000, 11790..11849)` and
  released, then one `tacli keys` batch of +3 notches at `(576,700)` followed by the pointer on
  the bottom edge pixel, so the anchor's steps and the engine's edge scroll race to the stop
  `extent − H = 11968`. Before the anchor's pre-clamp was made one-sided (`step_axis`), the log
  held 17 applied deltas that would have left the eye 2–10 px past 11968; after it, 6. Every one
  was clamped back on the same draw, every round ended with eye and target at `(3000, 11968)`, and
  since `step_axis` no step posts a pull-back (a step against the gesture's own direction).
* **The fog oracle** (`tagpu_fogwide_check.on`, 30×24 at 1024x768) read `differ=0` at every eye
  on the window builder it then checked: `compared=720 … skipped=0` at `(3000,3000)`, and
  `compared=616 … skipped=104` at both the NW corner `(−448,−352)` and the SE corner
  `(10304,12448)`, where two rows and two columns straddle the map's edge (the engine map, "The four
  border completions"). Not yet repeated on the fog source's window build.

### 2.3d The cursor, and where `u` is allowed to reach the engine (`zoom.on`, G13m)

**The engine is told the truth about where the pointer is.** `fake_GetCursorPos` answers `s`, at
every zoom. It used to answer the unzoomed `u`, because the engine's screen→world arithmetic is
1:1 and needs that number — but the engine also DRAWS its cursor from that poll, so the sprite
landed at `u` and the composite had to carry it back under the pointer. That job cannot be done
exactly: the surface texture the composite samples is only replaced when the game flipped, the
engine draws its cursor several times per flip, and any residual mismatch is multiplied by 1/z.
This gate's own first commit removed the largest term (a second, later sample of the pointer,
taken on the render thread) and halved the artefact; the rest was structural.

So `u` now reaches the engine in exactly two places, and neither is a poll:

| Path | Where | Why there |
|---|---|---|
| a **button** message's `lParam` | `tagpu_zoom_mouse_lparam()`, at the three `CallWindowProcA` doors | a button is queued on the engine's own event ring (`0x4B5EB2`/`0x4B5EFE` → `0x4C2E30`) and dispatched whenever the game loop reaches it. Its position is where the press was MADE, and no later pointer sample can reconstruct that |
| the **dispatched record**, at the mouse→world conversion | `vpw_mouse_world()`, our redirect of `0x498DA0` at `0x499221` | this is the one place the 1:1 arithmetic happens. It recomputes `u` from a single `g_ddraw.cursor` sample and writes it into `main+0x2C76` as well as the stack copy it is handed, because `GetUnitAtMouse 0x48CD80` (called at `0x499278`) and the routing test at `0x469DE1` read the field |

**The options/exit/preferences stack owns the point before either path can treat it as world space.**
F2 and Tab set `main+0x37EBE` bit 0 for `ARMOPT.GUI`; it stays set through
`EXITMENU.GUI`, `YESORNO.GUI` and the preferences screens while `DrawGameScreen` keeps
publishing the zoomed world underneath. The shared `to_engine()` door therefore reads the bit
before its geometric viewport test and leaves the point unchanged while it is set. This covers
hardware messages, injected button messages and the mouse→world repair with one read-only
gate; the DLL-owned render-options panel remains the separate `tagpu_menu_owns_point()` case
because it sets no engine ownership bit. Measured at 0.25× and 8×: `EXIT`, `MAINMENU`,
`EXITGAME`, both confirmation choices and preferences `OK` landed on their reported gadgets;
Resume cleared the bit and restored world input.
This is deliberately narrower than "a built-in in-game GUI": `SHARE.GUI` sets bit 6 at
`0x49374F`, not bit 0, and remains a known gap under zoom.

**A MOVE message must NOT be rewritten, and that is the half that took a measurement to find.**
`0x4B5E51` does not queue: it copies its record into `[obj+0x196]` through `0x4C2360`, and
`[obj+0x196]` is *also* a position the cursor is drawn at — `0x4C67C0`, called from the surface
present machinery, blits the sprite from that record without polling ([exe
RE](../exe-reverse-engineering.html)). With the poll answering `s` and the move still carrying `u`,
whichever ran last decided where the sprite appeared: measured at 1920×1080 / 0.25×, the sprite
tracked `u` across the frame at 4× the pointer's speed and off the left edge — the very artefact
this set out to remove. `carries_point()` is now the button messages only.

**Records that came off the ring are left alone, and it takes TWO tests to know which those
are.** The message id at `+0x10` is the obvious one, and on its own it is not safe: the input
reset `0x4B5A88` zeroes only the first three dwords of the record it writes to `[obj+0x196]`, so
time, msg and the double-click flag are left as stack garbage while the drawing polls put a real
pointer back in x and y. A garbage msg landing in `0x201..0x206` would make a poll record look
like a press and skip the repair — `main+0x2C76` left holding a SCREEN position while zoomed. So
the record must also **differ from `[obj+0x196]`**: the fallback is a `rep movsd` of those same
six dwords, garbage included, and can never differ from them, while a real ring entry differs in
at least its timestamp. *(Found by the landing review, not by the change.)*

**Armed by `tagpu_zoom.on` as well as `tagpu_vpwide.on` (each by file or by the play default,
§2.8), and WITHOUT EITHER THERE IS NO ZOOM.**
The repair is not optional once the engine is told the truth — and the zoom's own levers,
`tagpu_zoom.txt` and the wheel, are gated by no arm file at all (`zoom.on` installs the
minimap/scroll-step/camera-range patches and nothing more; the lever is read by
`tagpu_zoom_read_lever()` and the view published by the native pass, neither of which consults an
arm file). A build with `native.on` and `terr.on` but no `zoom.on` could therefore be wheeled to
0.5× into exactly the state this section exists to prevent: clicks landing correctly, because the
message carries `u`, while hover, the cursor-shape choice, build placement, `GetUnitAtMouse` and
the routing test all name the world under the SCREEN position. **So `tagpu_zoom_read_lever()`
asks `tagpu_vpwide_mouse_world_live()` and pins the level at 1.0 when the redirect is not in,
logging `zoom: PINNED AT 1.0` once.** The invariant is structural rather than documented, which
is what it needed to be — *found by the landing review, not by the change.*

With `zoom.on` and no `vpwide.on`, the redirect goes in and **nothing else does**: no rect is
ever widened, no other byte written. Verified live at 0.25× with `vpwide.on` absent: the viewport rect stays `(128 … 1919)`, the log reads
`vpwide: mouse->world repair only (0x498DA0)`, hover in the band is correct, and the ring stays
display-only exactly as §2.3b describes it without `vpwide`. At zoom 1
`tagpu_zoom_to_engine()` reports "nothing to do" and the stub writes nothing.

**What it closed.**

- **The widened hover, which was being clobbered every frame.** `main+0x2C76` followed the poll,
  so the wide `u` the window message correctly delivered was thrown away, and hover, the
  cursor-shape choice and build placement all named the 1× world point in the ring. Measured now,
  1920×1080 at 0.25×, `vpwide` armed, eye 2000: a commander whose 1× screen position is
  (956, 480) sits at screen (699, 525); `main+0x2C76` reads **(−276, 480)** and `main+0x2CBA`
  reads **unit 2**. The ring picks units up on hover, not only on click.
- **The right-edge scroll at zoom > 1** — §2.3c, measured there.
- **The composite's cursor machinery**, deleted: `uCur`/`uCurOff` and their branch in the
  composite shader, `CURSOR_PAD`, the pair recorder behind its seqlock, the poll filter, the
  latch at the surface upload, and `tagpu_zoom_to_engine_draw()`. The composite no longer knows
  the cursor exists.

**Measured, the same rig and scorer as the mitigation** (1920×1080, 0.25×, a real pointer crossing the band
at ~37 px per composited frame, three runs each), share of motion frames with the cursor left
behind at `u`:

| build | three runs |
|---|---|
| before the gate | 11 %, 10 %, 11 % |
| the composite mitigation (`13bc91c`) | 6 %, 7 %, 0 % |
| **this** | **0 %, 0 %, 0 %** (120–128 motion frames per run) |

Statically the sprite sits exactly under the pointer at (1100,570), (1600,600), (700,600) and
(300,500). Screen space is byte-identical at 1×, 0.25× and 2× — same `main+0x2C76`, same map
cell, same `main+0x2CC6` — over the minimap, the side panel and below the bottom bar. Box select
and click-select both pick the commander in the band at 0.25×, in the ring at 0.25×, and at 2×.

**Two gaps the landing review found and this did NOT close**, both pre-existing and neither
touched by the change:

- **The drag-scroll anchor drifts at zoom != 1.** `0x41CD50` subtracts a *screen*-space centre
  (`main+0x2CE3`/`+0x2CE7`) from a record that now reliably holds `u`, so the anchor is off by
  `64·(1 − 1/z)` px in x. It was equally wrong before, from a record that held whichever of `s`
  or `u` had landed last; the repair makes it *consistently* wrong rather than intermittently.
- **The whole repair is skipped when `[[main+0xC]+0xF1] & 8` is set**, because that bit gates
  `0x499A1C`, the call to the input-mode handler. What the bit means was not established.

**What it did not close.** In the ring the engine's own build-placement footprint is not drawn at
all: the engine projects it to a `u` off the surface and the clip discards it, so `markown` has
nothing to capture. Before, it was drawn — in the wrong place, from a wrong cell. The cell is now
right and the preview is absent; closing that means drawing the footprint ourselves, the same
answer `ui-markers.md` §6.1 gives for everything else outside the 1× viewport. **Dialogs drawn
over the viewport** (`ARMOPT`, `EXITMENU`) are unchanged and still take the transform as though
they were world, which was already true before this and is not verified either way here.
