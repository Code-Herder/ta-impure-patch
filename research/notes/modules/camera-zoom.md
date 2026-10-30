# Camera and zoom

Zoom, its live gate, zoom to the cursor, the widened viewport, the camera's range and the cursor at zoom.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.3 Zoom (`tagpu_zoom.c`, `zoom.on`)

| VA | What it is | Mechanism |
|---|---|---|
| `0x41C426` | `call 0x466B70` — minimap view rect, eye clamped at top | call-site redirect; the engine fills the rect, we rescale it by 1/z |
| `0x41C442` | `call 0x466B70` — ...and at bottom | call-site redirect |
| `0x430FAE` | `call 0x4B6A50` — the one site that persists `ScrollSpeed` | call-site redirect; substitutes the player's own value so our scaling can never reach the registry |
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
×1.163 in and ×0.877 out, and the target is clamped to `[1/8, 4]` in `iz` — 0.25..8 in `z` —
after each message, as the lab does.
**The ease is BAR's too**: a 250 ms tween on the QPC clock from the level currently drawn to the
target, `g = 1 − (1 − f)⁴` applied to `iz`, starting at the notch's own timestamp, so its length
is the same at any frame rate; a notch mid-tween retargets from the drawn level toward the
previous target × the new factor. A target inside `(0.999, 1.001)` **snaps to exactly `1.0f`**,
so 1× stays the byte-identical identity the transform, the minimap rect and the scroll rate all
test for by equality. At the end of a gesture, once the tween has landed, the log says
`zoom: wheel +120 -> 1.163, landed 251.7 ms after the last notch`. While the file is in force
the wheel is *pinned* to it, which is what makes deleting the file a handover rather than a
jump.

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

* **`vpwide`** never widened the addressable rect — `tagpu_vpwide_apply` takes `z = (c && c->live)
  ? c->zoom : 1.0f` — so every click and band box in the outer ring at zoom < 1 was dropped, the
  feature §2.3b exists for. Verified after: at `z = 0.513`, clicks on `armsolar` and `armrad`
  (1x screen x = 1110 and 1370, both off the window) select 1 unit each, the gap between them
  selects 0, and a band box over the ring takes the 2 units inside it.
* **The minimap view rectangle** stayed the 1x box. After: 8x7 px at 1.0, 14x13 px at 0.513.
* **The camera range** stayed the engine's at every zoom (§2.3c). After, edge-scrolled to the
  right edge: `eyeX` clamps at **9824** at 1.0 and **10063** at 2.144 — the closed form
  `9824 + (W/2)(1 − 1/z)` gives 10063.00.
* **`ScrollSpeed`** was never scaled, so scrolling at 0.25x crawled.

The **fog** is the one item that never depended on it: `tagpu_fogwide.c` builds every tick from
the screen size, and its oracle (`tagpu_fogwide_check.on`) reads `differ=0 engine-nonzero=300` of
720 cells at `z = 0.564` on an unmapped Two Continents — byte-identical to the grid the engine
built over its own window.

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

**Fog:** `bare=0` on every `fogwide` heartbeat of the run and the replication oracle
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
32 px at 1×, not to zero. `tagpu_fogwide.c`'s coverage derivation holds for a gesture that only
zooms in; one that reverses is held by the fog bound (below).

**The fog bound on the drawn eye [2026-09-24].** *The invariant:* every fog sample a frame takes
from a grid lies inside that grid's fully written cells — every drawn pixel's (`taFog`, at the
world point under it) and, on the wide grid, every CPU sample (`tagpu_fog_at`): the unit and wreck
gathers test the very point they sample against the gathers' slab, and the feature, effect and
particle gate (`tagpu_fx_tile_visible`) moves a point in the grid's unwritten last column or row onto
that band's written edge and answers every other point where it lies, so a laser gated on a head far
outside the view reads the fog the grid gives there — for any gesture, reversal or lag. *The bound:* `predict()` in `tagpu_zoom.c` chooses the grid together with the eye
the frame is drawn from, from the packet's own numbers. Both grids span `[org, org + 32·(cols −
1)]`: the builder fills entry `gx` from cells `gx` and `gx + 1`, so the last column is short its
right corners. The engine's grid is taken where it spans the view: level ≥ 1, its own window no
more than one cell past the map (`engine_grid_misplaced`, judged on the grid's origin and size, not
on the packet's eye — past that its border completions land off the map — and against the map's
PLOT grid halved into fog cells, `map_w16/2 × map_h16/2`, not the scroll extent, which is 32 px
narrower and 128 px shorter and would refuse the grid the engine builds at the bottom of its own
range), and the 1× rect about the
drawn eye inside its span. That holds for the eye it was built at, for every view: the builder
sizes the grid `view/32 + 2` cells, `+ 3` when the view is not a multiple of 32
(`0x483C1E..0x483C79`), and places it at the eye rounded to a half cell, so its span reaches
1..32 px past the far edge of the 1× rect for a multiple of 32 — at 1024x768 the grid is 30 × 24
cells for a 896 × 704 view (MEASURED, the descriptor at `*(main+0x1421F)`) — and `33 − m .. 64 − m`
px for a view `m` px past one: 34 rows and 9..40 px for the 1016 rows at 1080p, 68 rows and
17..48 px for the 2096 at 4K. A frame at rest therefore samples the engine's grid (`fog=engine` on
every run at 1080p and 4K), and an anchor step past that slack takes the wide grid. Otherwise the
wide grid, and the drawn eye `e` is clamped per axis until
the slab `[e + (vw − S)/2 − M, e + (vw − S)/2 + S + M]` (S = `tagpu_zoom_gather_span(vw, z)`, M =
`TAGPU_GATHER_MARGIN`, 256) lies inside its span. The window is that slab at the floor about the
eye it was built at, grown by **the lead**, a quarter of the viewport on each side
(`TAGPU_LEAD_DIV`), through `tagpu_zoom_pub_window`.

**The tables cover the fog reach, taken from the grids the packet carries and from no eye**
(`fog_reach` in `tagpu_packet_pub.c`). Per axis it is the union of what each way the bound can draw
a frame lets it reach: on the wide grid, its written span `[org, org + 32·(n − 1)]`, into which the
bound clamps the whole slab; on the engine's, taken only at level ≥ 1 where the slab is the 1× rect
plus M and only with that rect inside the span, `[org − M, org + 32·(n − 1) + M]`; and bare —
neither grid qualifies (no wide grid reached the render thread, and a level below 1 or a misplaced
engine grid), so no bound holds the eye, which is the packet's plus the steps not yet applied —
the window published about the packet's eye. Every slab the bound fits into a grid therefore lies
inside it, whatever moved the camera between the grids' builds and the publish. The units carried
with their pieces are those whose projected anchor (`wy − wz/2`, the pass's own test, so no bound
on altitude is needed) lies in the reach (`TAGPU_PK_U_INRECT`); one carried outside it that the
native pass finds inside its slab is not drawn and is counted, **`nopieces=`** on the `native:`
line — only a bare frame whose steps outran the lead, or a frame the bound could only centre
(`out=`), can make one. The feature anchors cover the reach plus 16 cells, scanned in whole rows
outward from `mid`, the centre row of the view about the packet's eye clamped into the rect, in
the order `mid, mid+1, mid−1, mid+2, …`: the scan stops at the first row
that does not fit in the room the table has left, and that row and every later one in the order
are dropped, the row at the same distance on the other side among them. An overflow of the
65 536-entry table therefore keeps a band `[mid − a, mid + b]`, `b − a ∈ {0, 1}`, and says so once
per scan (`trunc=` on the `feat:` line). A frame is drawn from the packet's eye plus the steps the
game thread has not applied yet, as far as the bound lets them through — on the wide grid at the
floor at most the lead (`vh/4`, 33 rows at 4K) and the lattice's rounding, since that grid is built
about the packet's eye. A frame loses nothing while the band holds its own slab at that offset
from `mid`, 560 rows at the floor at 4K. The scan is reused within
a tick only when the tick, the rect and `mid` all match. The densest stock map, Canal Crossing, puts 12 633 anchors in one rect at 4K
with the lead — every feature it has, 19 % of the table; next are Cavedog Links CC at 10 289 and
Plains and Passes at 9 603, each also whole; at 1080p Canal Crossing's worst rect holds 9 740
(the feature cells of the 275 stock maps' TNT files, every placement of the rect, 2026-09-24).
Wrecks made in play add to those counts. MEASURED 2026-09-24 on `feat-forest` at 1920×1080 on Xvfb,
the eye held at three places at 1×, 0.5× and 0.25×, against a diagnostic build whose table held
1 500: where the rect held more (1 494 and 1 499 kept of rects 393 rows tall), the scan kept rows
42..413 about centre row 219 and 117..505 about 294 — only the bottom rows, the farthest from the
view — and the frame's `anchors=` read 41, 89, 561 and 0, 38, 598, exactly the committed build's,
with `outside=0` on all 193 heartbeats and `trunc=` counting 1 081 frames. The interval
therefore holds every eye within the lead
of the build eye; the clamp is the identity for a gesture that only zooms in and for an unapplied
displacement inside the lead, and what it holds back is a larger one, for as long as that lasts.
The clamp reads the grid's own origin and size, never the lead. It is empty only for a
grid trimmed by a failed allocation, where the slab is centred and the frame counted. With no wide
grid in the packet a frame on a qualifying engine grid is held into it; any other is drawn bare —
unclamped over the engine's grid — and counted.

**The engine grid's origin is its build record.** The builder `0x4843C0` derives the origin from
the eye it reads (`32·col0 + 16`, `col0` the eye's half-cell-rounded `>> 5`) and stores it nowhere,
so the publisher keeps a record of the eye each build read, taken off LosType bit 3
(`main+0x14281`): only a build sets it — `0x484904` right after the lazy test `0x4848F2` and the
build `0x4848FA` inside the fog function `0x4848E0`, and `terr_fogtick` right after its own call —
every other store to the word either clears it or writes it back unchanged, and nothing
`DrawGameScreen` reaches through direct calls, branches or jump tables clears it or stores the eye
(engine map, "who sets `main+0x14281` bit 3", with the walk's method and what it leaves out). The bit is
read after the command apply, just before the draw, and again right after it: clear then set is a
build inside the draw at the eye it was drawn at, which the publisher records; set before is no
build, and the record stands however the eye has moved since — the camera shake `0x41C6F0` moves
it without clearing the bit; set before and clear after is not expected and drops the record
(**`recodd=`** on the publisher's `fog:` segment, must read 0). A draw the observer does not track
(the screenshot sweep, a nested draw, a draw past the return-stack depth) and a new level drop the
record, and while there is none the apply clears the bit, as every engine eye writer does, so the
draw rebuilds and makes one; the engine grid is not carried until it has (**`norec=`**, publishes
without a record). This holds with the fog site handed back to the engine as well — the record
comes from the bit, not from inside our replica — which is what the fog-site latch it replaced
could not do: that latch was void on a level's first draws and after a hand-back, and the fallback
was the packet's eye. MEASURED 2026-09-24 on Two Continents at 1920×1080 on Xvfb, the play set, a
72-unit fight in view with the screen shake on, then the commander followed (`Ctrl+C`) on a long
move so the stepper moved the eye every frame — from (−441, 7025) to (1483, 5384) — while the fog
site was handed back (`terr.on=passive`) and retaken at 1×, and `terr.on` dropped and re-armed at
0.5× (where a wide rect keeps the site ours): `nopieces`, `norec` and `recodd` 0 on every heartbeat
from the level's first, `fog=engine` at rest and during the hand-back, `wide` while the followed
eye stood past the map's west edge (where the engine grid is misplaced) and at 0.5×, and `bare`,
`out` 0. A diagnostic build (not committed) checksummed
the engine grid between records and compared each record with `terr_fogtick`'s own eye: 3 639
records, 2 947 of them at our rebuilds and every one at the eye that rebuild read, 692 from the
engine's own fog draw during the hand-back, and the grid's bytes unchanged across all 28 641
draws between records — no build went unrecorded. With the sim paused under `ARMOPT`, one
`tacli eye` jump made exactly one record: the apply's clear rebuilds the grid at the new eye. A
second level in the same process, entered through the menu below 1× on Seven Islands, read
`norec` 0 and `nopieces` 0 across the boundary, with `bare` and `out` at 18 from its first frames
(a level that starts below 1×, below) and flat after.

*The drawn view never moves against the gesture.* The clamp's upper end `org + 32·(cols − 1) −
(vw + S)/2 − M` falls as the level falls, so a zoom-out that cuts a zoom-in whose displacement the
game thread has not applied walked a held eye backward frame by frame and snapped it forward when a
packet landed — a sawtooth, one tooth per packet. When the bound would step the eye away from where
the gesture puts it, on either axis, the frame is drawn at the last frame's level instead: the zoom
pauses with the eye, and the eye moves only toward the gesture. The bound itself chooses the grid
and clamps the eye at that level, so the fog invariant is untouched. It is enough because, for one
packet, last frame's eye is inside that level's interval by construction, and a newer packet's wide
interval is non-decreasing in the eye it was built about (its window is that eye's floor slab
snapped down to the lattice, its size fixed by the viewport) — so it fails only when the grid's own
eye moved against the gesture (a step of another gesture, the engine's own camera writers) or the
grid was trimmed; then the step is taken, because the fog invariant is the one that cannot give,
and counted as `back`. The tween restarts from the level actually drawn, so a notch taken during a
pause does not jump.

*The rule needs an interval.* Where the one at this level is empty — a grid trimmed narrower than
the slab — the bound centres the domain on the grid, and a centred eye is not a step the gesture
asked for in either direction; pausing on it would hold the last level for as long as the trim
lasted. So the rule applies only when the bound fitted the eye into a non-empty interval (`fit` out
of `fog_bound`), and the retry is taken only when it fitted too; a centred frame is drawn as the
bound left it and counted in `out`. MEASURED 2026-09-24 on a diagnostic build whose wide grid was
sized at 60 % of the window (163 × 98 cells), Seven Islands at 1920×1080 on Xvfb, twelve notches
out at the view centre from 1× and twelve back: the level reached the floor, 0.25, with `paused`
0, `back` 0 and `out` 1 116 — the centred frames, held 16 px — and came back to 1.53 on the
engine's grid; with the rule applied regardless of `fit` (the build before) the level stopped at
0.4556 and `paused` grew by one every frame, 1 120 in two heartbeats, until the zoom-in released it.

The native heartbeat carries the witnesses — `fog=wide|engine|none bare=N out=N held=N/Mpx
paused=N back=N nopieces=N`: `fog` is the grid the last frame actually sampled; `bare` counts
presented frames that needed the wide grid and whose packet carried none, and must read 0 outside
`tagpu_fogwide.off`; `out` counts presented frames whose
domain is not inside the grid they took, checked on the pass's own numbers, and must read 0; `held`
counts frames the bound held back and the largest hold; `paused` the frames drawn at the last
frame's level; `back` the frames that moved against the gesture anyway, and must read 0;
`nopieces` the units inside a frame's slab that the packet carried outside its fog reach, and so
without their pieces, which were not drawn, and must read 0. `bare`, `out` and `nopieces` count
only frames that reached the screen: the pass gathers on every iteration of the render loop,
through the lane's bring-up, a swapchain rebuild and a skipped acquire, when nothing is presented,
so each frame's share is held until `tagpu_vk_frame` says it presented that frame
(`tagpu_native_presented`, from `render_vk.c`) and dropped by the next gather otherwise. The
publisher's `fog:` segment carries the build record's two: `norec` and `recodd` (above).

*Why a bound on the eye and not a bigger grid.* A grid spanning the whole reachable range is the
map plus half a view each side: on Seven Islands, the largest stock map (20480² px), ~547 k cells
and ~1.1 MB a packet at 1024x768, ~663 k and ~1.3 MB at 1080p, ~1 M and ~2 MB at 4K, rebuilt at
~2.5 ns a cell — 1.4, 1.65 and 2.5 ms of game-thread time per rebuild, at the sim tick rate
whenever anything moves. A window sized for the largest unapplied displacement is about four
times the floor's area, ~0.35 ms and ~280 KB at 1080p, ~1.4 ms at 4K. The bound costs a few integer
operations per frame and no byte of the packet; the lead below covers the one posted step the
prediction runs ahead by for 23–25 % more cells.

*Measured 2026-09-24* on Two Continents at 1024x768, aimed at the viewport corner facing the
interior, at the NW and SE corners of the camera's range and mid-map (5000, 6000). **The strip.**
`+3` then `−3` and `−3` then `+3` notches in one tween (one `keys` call), from 1× and from the
floor, with the game thread's apply forced 200 ms behind (a diagnostic build, not committed):
**before** — the old choice with no bound — `out` 7, 12 and 17 frames on the three `+3 −3`
gestures from the floor, the slab past the grid by up to 432, 514 and 427 world px (up to 226 px of
drawn view on the leading side, 57 screen px at 0.265×); **after** `out` 0 on every gesture, the
eye held for 15, 11 and 7 frames by up to 259, 515 and 457 world px. **The sawtooth.** A
per-frame log of the level, the unheld eye and the drawn eye over 36 gestures — the three places,
from 1×, from the floor and from 0.35×, `+3 −3`, `−3 +3`, `+3 −6` in one call and `+3` then `−6`
in two — counting a frame against the gesture when the drawn eye steps away from where the gesture
puts it: under the 200-ms lag, **before** 30 frames of 641, all on `+3 −6` from 0.35× (8, 11 and
11 at the three places), up to 55 world px back before the snap forward; **after** 0 of 631, with
32 frames paused (7, 11 and 14) and `back` 0. Without the forced lag (the shipped behaviour) 0
frames against the gesture before and after, and 0 paused: the game thread applies each step
before the next frame, and a grid built at the eye the last frame was drawn toward always covers
it. Without the lag, and before the lead below, the gestures below 1× still held the eye — 50
frames in all, by up to 263 world px (~66 screen px at 0.25×): the prediction runs one posted step
ahead of the game thread by design, and at the floor the slab had no slack beyond the grid's own
rounding, so the first steps of a tween waited a frame for the grid. On a held frame a click is
mapped through the eye the game thread applies, up to the hold from where it is drawn.

**The lead — built on the owner's call.** The published window grows by `vw/4` and `vh/4` on
each side, so that posted step no longer waits for the grid. MEASURED 2026-09-24 on Seven Islands
with the play set, the same 36 gestures at NW, SE and mid (10240, 10240), counted from the
heartbeat's `held`:

| | cells (bytes a packet) | build, mean / max per rebuild | held frames, largest hold |
|---|---|---|---|
| 1080p, reference GPU, before | 36 260 (72.5 KB) | 72 / 206 µs over 533 rebuilds | 53, 509 world px |
| 1080p, reference GPU, with the lead | 44 772 (89.5 KB), +23 % | 91 / 261 µs | 4, 61 world px |
| 1080p, Xvfb, before | 36 260 | 104 / 320 µs over 220 rebuilds | 24, 949 world px |
| 1080p, Xvfb, with the lead | 44 772 | 99 / 249 µs over 257 | 11, 474 world px |
| 1080p, Xvfb, with the lead, apply forced 200 ms behind | 44 772 | 105 / 280 µs over 107 | 33 (11 paused), 779 world px |
| 4K, Xvfb, before | 137 255 (274.5 KB) | 313 / 720 µs over 62 rebuilds | 13, 2511 world px |
| 4K, Xvfb, with the lead | 171 588 (343.2 KB), +25 % | 386 / 840 µs | 8, 1578 world px |

The reference-GPU rows are a diagnostic build that grew the fog window alone, the same window the
committed build publishes; the Xvfb rows at 1080p are the committed build and the one before it.
On Xvfb llvmpipe draws a handful of frames a second, so each held frame there spans several posted
steps: its cells, bytes and build times hold, its hold counts compare only with each other. Every
run read `back`, `out` and `bare` 0, and the feature pass's `outside=` 0 on every heartbeat (371
and 369 of them on the committed build and the one before). Re-run on Xvfb at 1080p after the
tables moved to the fog eye and the pause rule learned to need an interval: 13 held frames, the
largest 497 world px; with the apply forced 200 ms behind, 42 (9 paused), 779 px; `back`, `out`,
`bare` 0 and `outside=` 0 on 372 and 371 heartbeats. Re-run again after the tables moved to the
fog reach and the engine grid's origin to its build record: 15 held frames, the largest 511 world
px; under the lag, 28 (5 paused), 740 px; `back`, `out`, `bare`, `nopieces`, `norec`, `recodd` 0
and `outside=` 0 on 372 heartbeats each. The rebuild rate is ~2.8/s during the
gestures, so the lead adds ~50 µs of game-thread time a second at 1080p. The units' piece-cull
rect and the anchor rect are the fog reach, which holds the wide grid's span, so both grow with the
lead; the anchor rect's margin is 16 cells past the reach.

**The wide grid is built on every tracked in-play draw of a Vulkan session**, whoever owns the
fog site. `terr_fogtick` ticks it at the engine's fog call `0x469D8E` on a draw latched to our
terrain; on any other draw the engine's own `0x4848E0` runs there, and the publisher ticks the
grid in that draw's `after` (`wide_tick`), before the fill copies it, telling it `rebuilt` when
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
the GDI lane. On the GDI lane nothing reads the wide grid, so nothing builds it: `wide_tick`
runs only while the session's renderer is Vulkan (`g_ddraw.renderer`, stored on the game thread
at DirectDraw creation before the first draw and moved later only by the Vulkan backend's
hand-over to GDI). That is a property of the session, not the per-frame zoom gate the tick rules
out (`tagpu_fogwide.c`: a producer gated on the level the render thread publishes builds one
frame late at the start of every zoom-out). MEASURED 2026-09-24 on `big-battle` at 1920×1080 on
Xvfb with the play set and `tagpu_gdi.on`: the build before allocated the 273×164 grid (87 KB)
and ticked it on every draw (3 086 ticks in its first five-second heartbeat); this one logs no `fogwide:`
line. With `terrown.off` on the Vulkan lane, Two Continents six notches out (0.456): `fog=wide`,
`bare`, `out`, `back`, `nopieces` 0, the grid rebuilt 29.7 times a second. MEASURED 2026-09-24 at 1920×1080 on Xvfb with the play set, Two
Continents zoomed out six notches (0.456), the level ended through the menu (EXITMENU →
MAINMENU) and a skirmish started, the zoom still below 1 (the second level's heartbeat reads
`fog=wide`): before, `bare=17 out=17` on the second level; with only the presented-frame rule
below, 2 — the frames that reached the screen; with the tick, 0, and `back`, `nopieces`, `norec`,
`recodd` 0. The world A/B fixture, which holds `terr.on=off native.on=all` at 0.5× for five
seconds, read `bare=283 out=283` before and 0 after, on the reference setup's GPU at 1024x768.

**A packet cut at a level's start was drawn with no fog.** MEASURED 2026-09-24 on `big-battle` at
1920×1080 on Xvfb, a fresh instance with the play set and no input: one frame counted `bare` at
the level's start, the first heartbeat reading `fog=engine bare=1` and the publisher `norec=0
recodd=0`. A diagnostic build (not committed) logged it: frame 995, tick 42, level 0, a packet
published with `truncated = 0xfc6d` — the font, units, pieces, anchors, shade, particle, light,
both fog grids, fog shade and minimap all cut — because the scenario's units arrived and the fill
outgrew the write slot's committed pages. The frame packet exchange published the cut fill and
grew the slot for the next one, so the native pass drew that frame without its units and with no
fog grid (`fog=none`): a wrong frame, not an empty one. It had nothing to do with the camera; any
level whose tables outgrow the slots does it. The exchange now grows the slot and fills it again
before publishing, and the fill reports its whole size (§2.16, the primitive), so no frame is
published without the tables it asked for while the slot can grow. After, on the same run:
`bare=0 out=0 back=0 nopieces=0`, `norec=0 recodd=0`, packet `trunc=0 refill=3 grow=15`. Two
Continents, the fight / follow / hand-back run above: `bare`, `out`, `nopieces`, `norec` and
`recodd` 0 at every stage, `back=0` on every heartbeat, `trunc=0 refill=3`. Re-run with the wide
grid ticked on every draw and the per-publish counts: `big-battle` `bare=0 out=0 back=0
nopieces=0`, `trunc=0 refill=4`, the world segment's `trunc=0/0/0/0`; Two Continents 0 on every
stage, `trunc=0 refill=2`. **Past the reserve** (a diagnostic build with a 512 KB frame reserve,
not committed, `big-battle`): with the loop that stopped on `slot_commit`'s answer, one frame
refused by the consumer (`viol=1`, `cap_bytes != the slot's capacity`); with the loop that stops
on what it committed, `viol=0`, every publish past the reserve counted (`trunc=432`), the units
kept (`units=948`) and the fog grids the layer cut, so 247 frames were drawn bare. **The fog grids
and the shade table now go down ahead of the world tables**, the reserve holding them at the
design point, so an overflow cuts what follows: at the same 512 KB, which cannot hold the grids
and `big-battle`'s world tables together (about 600 KB), both grids are carried and `bare=0`, and
the unit table is what is cut; at 640 KB, which holds those but not the whole fill (683 KB), the
grids and every world table are carried (`units=959 pieces=14254 wrecks=13`), the cut lands on
the light layer and the minimap (`truncated = 0x8800`, 428 publishes), and `bare=0 viol=0`. Re-run
on that build: `big-battle` `bare=0 out=0 back=0 nopieces=0`, `trunc=0 refill=4`; Two Continents
0 on every stage; the second level below 1× 0; `big-battle` under
`check` + `stress`, where every in-play draw publishes into one-page slots that must grow:
`trunc=0 refill=12 grow=43 viol=0 pviol=0 crcbad=0` (after the fill order, `trunc=0 refill=5
viol=0 pviol=0 crcbad=0`). Merged with main's full-colour Classic and run on the reference
setup's GPU at 1024x768, the world A/B with the eye pinned (terrain at 1×, 2× and 0.5×, units at
1× and 2×) read main's own build's md5 on all five captures, main, this branch and main again on
one instance.

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
would need an off-centre scale centre, which vpwide's addressable rect, fogwide's window and
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
of every gesture. A camera hold that is *moving* (a scripted pan through `tagpu_eye.txt`) now
bumps the fog sequence every frame, so such a pan draws on the wide grid throughout even at 1×;
the replication oracle reads `differ=0`, so the picture is the same, but the fog texture is
re-uploaded on each switch. And `s_eyeHold` is polled every 15 frames, so the hold gate is up to
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
grid's origin is its build record, read off LosType bit 3 whoever builds it (§2.3e), and the
wide grid is refused unless it was ticked in the same draw, at our fog site or by the publisher.

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
overshoots the map by at most one cell — `col0 ≥ −1` and `col0 + cols ≤ PLOT_C/2 + 1` per axis,
which an eye in `[0, extent − W]` guarantees (engine map, "The four border completions"). The
native pass therefore draws from `fogwide`'s grid on any frame whose engine grid breaks that —
tested on the grid's own origin and size, since the packet's eye need not be the eye it was built
at, and against the map's PLOT grid, not the scroll extent — as well as below 1× and on a frame
whose drawn eye the engine's grid does not span: the fog bound (§2.3, "The fog bound on the drawn
eye"). MEASURED 2026-09-24, Seven Islands at 3840×2160 on Xvfb with `fogwide.off`, the eye held at
1× at the engine range's SE corner (extent − view = 16736, 18256), at (0, 0), at the SE corner
again and 20 px inside it: `fog=engine bare=0` at every stop. The build that judged the grid
against the extent refused the grid the engine builds there and drew those frames bare — `bare`
585 at the first SE stop, 1 195 by the end.

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
  builds either fog grid, then recomputes the minimap box, sets its dirty bit and clears bit 3 of
  `main+0x14281`, whether or not the sim ticks. The first packet carrying the new eye therefore
  carries both grids built at it. Measured 2026-09-24, `camonly.json` (a camera line and nothing
  else) applied at rest after the eye was parked at (6000, 6000): the first frame drawn from the
  new eye (2376, 726) was not held, sampled the engine's grid, and its packet's wide grid had the
  new eye's origin (720, −624); the fog oracle read `differ=0` from then on and the heartbeat's
  `held` did not move. Written from the flip observer instead, as it was until then, the eye
  reached a packet whose grids were built at the old eye, and the fog bound drew that frame short
  of the target — by the landing review's arithmetic up to ~1376 px at 1× and a 896-px view.
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
* **The fog oracle** (`tagpu_fogwide_check.on`, 30×24 at 1024x768) reads `differ=0` at every eye:
  `compared=720 … skipped=0` at `(3000,3000)`, and `compared=616 … skipped=104` at both the NW
  corner `(−448,−352)` and the SE corner `(10304,12448)`, where two rows and two columns straddle
  the map's edge (the engine map, "The four border completions").

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
minimap/`ScrollSpeed`/camera-range patches and nothing more; the lever is read by
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
