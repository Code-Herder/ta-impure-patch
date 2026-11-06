# Log — the Vulkan-only plan, landings 4 and 8–10

Landing records; the plan is [vulkan-only-plan](../vulkan-only-plan.md).

*A frozen record: nothing new is written here. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.48 The lane becomes a backend, and its surface goes on the game window — landing 4a of the Vulkan-only plan

**What this part is, and what it is not.** `render_vk.c` gives the Vulkan lane a render thread of
its own — `vk_render_main`, dispatched from `dd.c` on `tolower(g_config.renderer[0]) == 'v'` — and
`tagpu_vk_own_present()` puts the surface on the window `tagpu_vk_frame` is handed instead of on a
window of route D's. It calls **no gather half**, so no pass has a hand-over and the frame is the
seam's clear colour and nothing else. That is the part's whole claim, and it is measurable on its
own: whether a swapchain lives on the game's own window *inside the game*, which nothing had run.

| what ran | result |
|---|---|
| `renderer=vulkan`, **no lever file present** | `up in 418 ms … our window 00020058 over 00020058, 640x480 … route E: the surface is on the game window and there is no GL lane` |
| an X grab of that game window | **307 200 of 307 200 px** at the lane's clear colour (255, 0, 255) |
| `renderer=openglcore` + `tagpu_vk.on` (the control) | `window: created 00010074 over 00030054 (route D)` — unchanged |
| that control's `tacli glshot` | **148 distinct colours** — the GL lane's picture is whole |

**100 % of the window being ONE colour is the correct result here and not a defect**, and this
note says so because §2.35's lesson is the opposite shape: *"0 px over content that is not there
is not a measurement"*. The reading that would have been wrong is a *partial* one — some of the
window the lane's clear and some of it stale desktop or the engine's own blit. What the two
handles being equal (`00020058 over 00020058`) adds is that it is the **game's** window rather
than one of ours, which is the only thing route D could not do.

**Route E is now measured in the game as well as in the probe**, and the difference matters
because this seam has already had an API say yes while the screen said no. `tools/vkcoexist.c`
route E (§G19a in [roadmap](../roadmap.html)) answers it on a bare top-level window; this answers it
on TA's own window, with the fork's window management, the input shield and the engine's message
pump all in play.

**What the lane costs in the real process, which the probe understates by ten times.**

| | committed peak | largest free block |
|---|---|---|
| before bring-up | 36.1 MB | 255.5 MB |
| with Vulkan up | 59.5 MB | 255.5 MB |

So **23.4 MB**, against the **2.2 MB** the 320×240 clear-only probe reads. Both figures are
right about different questions and the probe's own caveat said which: it bounds the *bring-up*
footprint, not the running one. The invariant survives the move — **the largest free block does
not change**, in the game as in the probe, which is the number that matters in a 32-bit process
because TA fails by failing to allocate rather than by saying anything.

**Route D is unreachable rather than disabled, at two places, and the second one is the point.**
The `ST_OFF` branch never asks for a window when the present is owned, so `s_vkwnd` stays NULL
and every `vkw_*` path is already inert through its own `if (s_vkwnd)` guard. That is
unreachability *by inspection*, which is the kind of argument this lane has been wrong about
before — so `tagpu_vk_wndproc` returns early as well, and a `VKW_CREATE` that was already in the
queue when the latch was set cannot create one behind us. The latch is one-way: which backend the
process has is settled at `dd.c`'s dispatch and cannot change, and a flag that could go back
would permit a surface on the game window and a route D window at once.

**Three things the seam had to be told, each of which would have been a live bug:**

* **"Our window went away" is not a case when we have no window.** The rebuild trigger tested
  `s_vkwnd != s_vk.hwnd`; with the present owned, `s_vkwnd` is NULL for the process's life while
  `s_vk.hwnd` is the game window, so that test would have been **permanently true** — the lane
  tearing itself down and rebuilding on every single frame, for ever. The same class as the
  comment two lines below it, which records the earlier version comparing the caller's `w`/`h`
  against the swapchain extent and rebuilding every frame because the two are different numbers.
* **The lever has to retire, in both directions.** `tagpu_vk.on` must stop arming the lane
  because `renderer=vulkan` already did; `tagpu_vk.off` must stop *disarming* it, because with no
  GL lane behind it a disarmed Vulkan lane is a black window rather than a fallback. The ON file
  is still read for its `color=`, which every A/B on this plan uses.
* **A log line that claimed something about a lane that is not in the process.** The bring-up
  ended `- route D: the GL lane's window is untouched` unconditionally. It now names the route it
  came up on, which is also the only thing distinguishing `our window X over X` from a mistake.

**The GDI fallback is allowed to be late, and that is a measurement rather than a preference.**
`tagpu_vk_failed()` reads the lane's own `ST_FAILED`, published after every path the bring-up
could have succeeded on — a fact, not a frame count. Handing the session to `gdi_render_main`
after a surface has been attempted on the HWND is sound only because route F measured GDI as
still reaching the screen there; had it not, the fallback would have had to be taken *before*
`vkCreateWin32SurfaceKHR` and any later failure would have been terminal.

**`dd.c` gets the dispatch and nothing else, checked site by site rather than assumed.** Every
other `g_ddraw.renderer == ogl_render_main` test in that file is a WGL workaround a swapchain
must not inherit, and adding `|| renderer == vk_render_main` to any of them would be silent:

| site | what it does | why Vulkan must not have it |
|---|---|---|
| `dd.c:850`, `:995` | `nonexclusive = TRUE` | stops WGL taking fullscreen exclusive; its one consumer is `:1120`, so GL-only here means exactly "no extra scanline" |
| `dd.c:1120` | `render.height++`, `opengl_y_align = 1` | a scanline added so the driver cannot take exclusive mode, plus the viewport shift paying for it. `opengl_y_align` is read in `render_ogl.c` and nowhere else — a Vulkan frame inheriting it would be one pixel too tall and offset |
| `dd.c:1265`, `:1354` | `ogl_create()`, GDI on failure | the Vulkan bring-up is the render thread's own |
| `dd.c:1525` | `SetPixelFormat` | already GL-gated, and route E is measured on a window without one. The HDC beside it is taken unconditionally, which is what leaves the GDI fallback a valid one |
| `dd.c:1788` | `ogl_release()` | nothing to release, and `dd.c` joins the render thread before reaching it, so `tagpu_vk_render_stop` has already run |
| `fps_limiter.c:153` | `fpsl_dwm_flush()` instead of the vblank wait alone | **outside `dd.c`, and on this backend's per-frame path.** A Windows-7 DWM workaround; `!IsWine()` makes it inert on the reference setup, and the Vulkan present does its own pacing |
| `winapi_hooks.c:2039` | `ogl_release()` in `fake_DestroyWindow` | nothing to release — and that function is the **only** writer that nulls `g_ddraw.hwnd`, which is why the `s_ownGone` latch exists rather than trusting `hwnd != s_owner` |

**The line numbers above are post-change, and the first version of this table had every one of
them off by one** — adding `#include "render_vk.h"` at `dd.c:12` shifted the file under an audit
recorded against the pre-change numbers, so four of the seven cited a `}` or a blank line. Caught
by the landing review. The scope was wrong too: the audit said "in `dd.c`" while one of these
sites is in `fps_limiter.c` and runs every frame.

**Not covered by landing 4a:**

* **No gather half runs**, so no pass draws — that is 4b, and it is where the per-pass previous-build
  A/B against `0b5e06d` gets taken. `tagpu_overlay_draw` is the single driver and most of it is
  API-independent; the GL-owning entry points are `tagpu_scaffold_frame`, `tagpu_native_frame`,
  `tagpu_gui_present` and `tagpu_fps_present`.
* **`TAGPU_FRAME` is not filled**, and 4b must fill it *before* calling `tagpu_vk_frame` with the
  same frame number — a pass refuses a hand-over stamped with any other frame. Note `vp_y` takes
  `viewport.y` **without** `opengl_y_align` on this path.
* **`ss` has no target and TA's surface is not uploaded by the backend** — 4c.
* **Route D's window, `tagpu_vk_wndproc`, `WM_TAGPU_VK` and the geometry tracking still exist** —
  4d deletes them, and only after 4b's figures are banked.
* **The GPU row cannot retry after the GDI fallback**, and this is a real loss rather than an
  oversight. While the lane was a lever beside GL, a failed bring-up retried when the player
  picked another device (`lane_gen() != s_choiceSeen`). Once the session is on GDI the thread is
  GDI's for the process's life. The alternative — sitting in the loop presenting nothing while the
  player hunts for a device that works — leaves them with no picture at all.
* **One launch, one GPU, one driver.** Every figure above is the linux NVIDIA ICD under system
  wine at 640×480 in the shell. Nothing here speaks for Windows, for llvmpipe, or for a frame with
  a world in it.

**Three residuals the landing review named and this landing deliberately did NOT fix**, each with
the reason, because shipping a fix that cannot be measured is not a landing here:

* **`WM_ERASEBKGND` is not claimed on the game window.** Route D's own window proc returned 1 for
  it — *"we paint every pixel from the present; GDI must not flash over it"* — and
  `g_ddraw.hwnd` has no such protection: the fork's `fake_WndProc` only sets `clear_screen` and
  falls through, and `dd.c:1445` issues a `RDW_ERASE | RDW_INVALIDATE` on every mode set. So an
  erase can paint over the swapchain's last presented image until the next present. **The GL
  backend has the same exposure**, so this is inherited rather than introduced — and 4a's frame is
  a flat clear, so a flash is not observable in it: an X grab of 307 200 identical pixels is
  consistent with a window that flashes. **It belongs to 4c**, where the backend owns TA's surface
  and there is content for a flash to interrupt, which is the first point at which the fix can be
  shown to work.
* **The GDI fallback's on-screen banner blames OpenGL.** `render_gdi.c` prints *"please update your
  graphics card driver (%s)"* with `g_oglu_version`, which is the zero-initialised array on this
  path because `dd.c` deliberately never calls `oglu_load_dll()` for `renderer=vulkan` — so it
  reads *"…driver ()"* and names the wrong subsystem. Safe, and the `TRACE` beside it is correct.
  Rewording a shared banner belongs with landing 11, which deletes GL and makes the OpenGL wording
  wrong everywhere rather than only here.
* **The zero-extent bring-up retry is now unconditional.** `up_worker`'s "the window has no extent
  yet" path returns the lane to ST_OFF and the next frame tries again; with the lever gone that
  spin is no longer gated on arming, so a span with a zero client rect costs one instance and
  device creation per ~420 ms for as long as it lasts. **Left as it is on purpose**: the retry is
  what gets the lane up once the window has an extent, and a bound that stopped retrying would
  wedge the lane for the session — a worse failure than the waste.

### 2.49 The gather halves run with no context, and the A/B learns to arm itself — landing 4b-1 of the Vulkan-only plan

**What this part is.** 4a's backend called no gather half, so no pass had a hand-over and the
frame was the seam's clear colour (§2.48). This part wires `tagpu_overlay_draw` into
`vk_render_main` and then teaches two of the driver's four GL-owning entry points to run their
gather and stand their draw down: `tagpu_scaffold_frame` and `tagpu_fps_present`. It also fixes
the thing that made *any* of this unmeasurable — the Vulkan capture was armed by the GL capture
having reached the disk.

**The driver first, and one frame number for the whole frame.** `vk_render_main` fills a
`TAGPU_FRAME` and calls `tagpu_overlay_draw` before `tagpu_vk_frame`, so everything the fork adds
that is not a GL draw is live under `renderer=vulkan`: the input injection, the trigger readers,
the own-draw flushes, the palette, the zoom, the packet exchange and the reclaim bracket. Two
details in it would each have been a silent bug:

* **The frame counter is taken once, before anything uses it.** A post-increment inside either
  the driver call or the present call would have differed by one, and a Vulkan pass refuses a
  hand-over stamped with any other frame — so the symptom would have been every pass standing
  down, not an error.
* **`vp_y` takes `render.viewport.y` WITHOUT `opengl_y_align`.** That is the extra scanline WGL is
  given so the driver cannot take exclusive mode (`dd.c`, and §2.48's audit); carrying it would
  offset every Vulkan frame by one pixel, and no capture would catch it because both halves of a
  two-lane A/B would carry it equally.

`surface_tex` is 0: there is no GL texture to name, and 0 is what the fork already hands a
non-8bpp frame, so it is in-contract rather than a sentinel. Its consumers have nothing until
4c takes over TA's surface upload.

**AND THE COUNTER HAS TO OUTLIVE THE THREAD, which the first version did not.** It was a local,
so it restarted at 0 — and `dd_SetDisplayMode` joins the render thread (`dd.c:747`, INFINITE) and
`dd.c:1459` creates a new one, so every mode change replayed the whole sequence of frame numbers.
The GL lane's `g_tagpu_frames` is a file static (`render_ogl.c:47`) and never repeats. This
matters because **every hand-over in the tree tests freshness by exact equality on that number**
and says so in as many words — `tagpu_terr.c`'s *"THE STAMP IS WHAT MAKES THE POINTERS ABOVE
SAFE"*, and the same test in `tagpu_fx.c` and `tagpu_posedraw.c`. Equality against a counter that
restarts is not a freshness test: a record published on the frame the old thread died and never
collected would match again that many frames into the new thread's life, and the twin would draw
from pointers into buffers freed and rebuilt in between. Latent exactly one commit out, because
the passes carrying those stamps are the ones 4b-2 ungates. [Found by this landing's review,
2026-09-18; now a file static, as on the GL lane. One backend is chosen per process and can only
degrade to GDI, so the two counters can never both be live.]

**The GL calls are not left to no-op, and that is a decision rather than a tidiness.** With no
context current most of them do nothing. The ones that do not are the ones that read GL state
back: `tagpu_scaffold.c`'s `init_gl` branches on a `GL_COMPILE_STATUS` and a `GL_LINK_STATUS`, so
a lane with no context takes whichever branch the loader's stubs produce, and *both* are wrong —
one logs a shader failure that never happened, the other latches `s_state = 2` and the pass is
dead for the life of the process. A pass that is not called publishes nothing instead, and its
Vulkan twin then refuses out loud (`no hand-over for frame N — the GL pass published nothing`).
The gate went in at the driver in 4b-1's first commit and moves inward one pass at a time.

**Where the line falls inside a pass.** Not at the pass, at the point where the pass stops being
API-independent:

| pass | runs on both lanes | stands down under `renderer=vulkan` |
|---|---|---|
| `tagpu_scaffold_frame` | the packet's anchors walked over the engine's sweep rect into `s_buf`, the per-unit occlusion prediction read back out of it, the four NDC numbers | `init_gl`, the `GL_R8` upload, the blended quad, `tagpu_abshot_*` |
| `tagpu_fps_present` | the lever poll, the 500 ms averaging window, `tagpu_text_frame`'s font latch, the `emit` loop packing eleven cached strings into `s_v` | `init_gl`, the `glBufferSubData`, the triangles, `tagpu_abshot_*` |

Both publish their hand-over **once**, outside the GL block, because the hand-over belongs to
both lanes; in the GL-only code it sat inside the draw.

#### The A/B's arming, and why it had to move first

**The Vulkan capture was armed by the GL capture reaching the disk.** The coupling is deliberate
and documented (`tagpu_abshot.h`): `tagpu_abshot_end` returns 1 only when the PPM was written, and
the caller must not claim the Vulkan half on anything else — because a refused GL write leaves the
PREVIOUS run's `_gl.ppm` lying on the disk, and a Vulkan half diffed against that reports a
capture of a different frame as a port failure. An oracle failure wearing a port failure's
clothes, which is the worst answer an oracle can give.

**On the vulkan-only lane there is no GL half to ask.** `tagpu_abshot_end` is not merely refused
there — it is never called, because the pass that would call it has stood its draw down. So
`wrote` is 0 on every frame and the rule refuses every capture **on the only lane that presents**.
The chain, for the scaffold: `tagpu_scaffold.c:324` polls the trigger → `taking = s_ab &&
!s_abDone` → `s_abFrame = tagpu_abshot_end(...)` → `tagpu_scaffold_overlay(..., &ab)` →
`tagpu_vk_scaffold.c`'s `prepare` → `s_abFrame = ab`. Landing 4's own oracle is a comparison of
two **builds** and has no `_gl.ppm` in it at all, so the guard that makes the two-lane oracle
trustworthy is exactly what prevents the two-build one.

**So the arming is claimed on the intent, and what the rule was buying is established by
construction instead.** Where both lanes run the rule is unchanged and is still what a pass
applies. Where only one does, `ab = taking`, and the target `_vk.ppm` is unlinked — by
`tagpu_vk_ab_arm(tag)`, which the pass calls **in the same statement sequence that latches the
claim**. After that call the file does not exist; it comes back only if `tagpu_vk_shot_finish`
writes it. **Absent means this arming produced no capture** and **present means this arming's**,
on either lane. The eight names live in one table (`s_abFiles`) that both the write path and the
arming read, keyed by the same tag the GL half already passes to `tagpu_abshot_end`, so the name a
capture is written to and the name an arming unlinks cannot drift.

**WHERE the unlink lives is the whole guarantee, and the first version had it in the wrong place.**
It was in `vk_present`, beside the capture it protects, which looked equivalent and was not: the
claim is latched on the gather side whether or not the lane ever collects it, and `vk_present` is
not on the path from that latch. `tagpu_vk_frame` returns before it all through the ~250 ms
bring-up, on a window or GPU-row rebuild, at `ST_FAILED` and at `ST_ZOMBIE`, and `vk_present`
itself returns early on an out-of-date acquire. So the unlink ran on most frames and was missed on
precisely the frames where no capture happens — which is the case it exists for. A guarantee about
timing wearing the words of one about construction, in a landing whose whole subject was that
distinction. [Found by this landing's own review, 2026-09-18; the fix is the placement above,
where one thread reaches both in one basic block and no interleaving separates them.]

**And the unlink's answer is the claim's answer.** `DeleteFileA` can fail — a reader holding the
file open, or a read-only file — so discarding its result left "the file does not exist" asserted
rather than established. `tagpu_vk_ab_arm` returns 1 only when the target is gone (already absent
counts), the pass claims on that as well as on `end`'s, and a failure is named:
`vk: ab: tagpu_scaffold_vk.ppm could not be removed (error 5) - this arming is REFUSED`. The
stale file is then still there, which is the residual — but no claim was granted, no capture was
written, and the log says so.

That is a fact about the filesystem rather than an argument about ordering, which is all the old
rule could have been once the half it read stopped existing. It also closes the refusals that used
to leave a stale `_vk.ppm` behind where both lanes run: a surface whose images do not carry
`TRANSFER_SRC`, two levers armed in one frame, and every path on which the lane never presents.
Until now those were handled by the operator's `rm` in the recipe, not by the code.

#### Measured — one build, one fixture, three runs

`feat-forest` on Two Continents at 1024×768, `ss=1`, one `.ab` at a time. The camera is
`[2950, 1010]` / eye `(2566, 616)` in **all three** runs, and the scaffold's own gather figures are
identical in each: `swept 68x76 tall=151 flat=30 gafFallback=1 junk=0`.

| pass | run | result |
|---|---|---|
| scaffold | `renderer=openglcore` + `tagpu_vk.on` (route D) | **0 px apart**, 190 247 ink px a side |
| scaffold | `renderer=vulkan` (route E) | `tagpu_scaffold_vk.ppm` **written**, and **byte-identical** to the two-lane run's Vulkan half (same md5, 0 px, 190 247 ink px) |
| fps | `renderer=openglcore` + `tagpu_vk.on` (route D) | **0 px apart**, 102 ink px a side |
| fps | `renderer=vulkan` (route E) | `vk: fps: the Vulkan edition is up — 512x256 atlas`, `tagpu_fps_vk.ppm` written; 17 px of 786 432 against the two-lane run's Vulkan half |

**The scaffold's byte-identity is the strong result, and the fps readout's 17 px is not a
weaker one.** The scaffold is a pure function of the eye, the viewport and the map's features, so
two runs at the same camera must agree exactly, and they do. The readout draws **its own
measurement**: the two runs ran at different frame rates, so the digits differ and nothing can
make them agree without measuring the clock instead of the renderer. What makes that a result
rather than an excuse is *where* the 17 pixels are — the ink bounding box is `x 6..50, y 8..14` in
both, the differences are confined to `x 32..43`, and columns 0..31 (the `FPS` label and the
leading digit, 57 ink pixels) agree **exactly**.

**The capture existing is also what proves the gate held.** Had `init_gl` run without a context it
would have latched `s_state = 2`, the pass would have published nothing, and the twin would have
had no hand-over — so there would have been no `_vk.ppm` to diff.

**The unlink is tested on the paths where it is visible, and a succeeding capture is not one of
them** — it overwrites the file anyway, so the positive path says nothing about it. Three
refusals, each with a sentinel planted as `tagpu_scaffold_vk.ppm` first:

| the path | what happened |
|---|---|
| two world passes drawing, one lever armed | the lane refused (`1 A/B levers claimed this frame and 2 passes drew into it - nothing captured`) and the sentinel was **gone** |
| the lane down (`tagpu_vk.off`), so `vk_present` is **never reached** | the sentinel was **gone** — this is the path the first version missed, and it is deterministic rather than a race |
| the target read-only, so the unlink fails | `vk: ab: … could not be removed (error 5) - this arming is REFUSED`, no `shot: wrote` followed, and the claim was not granted |

The middle row is the one worth keeping: with the unlink in `vk_present` the sentinel would still
have been there and `vk-ab.py` would have read it as this arming's capture.

#### Not covered by 4b-1

* **`tagpu_native_frame` and `tagpu_gui_present` still stand down whole**, so under
  `renderer=vulkan` there is still no world and no UI layer — the window is the clear colour with
  the scaffold's overlay or the readout on it when those levers are armed. §2.48's *"100 % one
  colour is the correct result"* therefore still holds for an unarmed instance and no longer holds
  for an armed one.
* **The two remaining entry points are not two more of the same job**, which is what pushed 4b
  into two landings — see [vulkan-only-plan](../vulkan-only-plan.html) landing 4b for the seam. In
  short: each world pass publishes its hand-over from **inside** its GL render
  (`terr_publish` is called from `tagpu_terr_render`, not from `tagpu_terr_gather`), so the five
  world passes with a lever of their own — `terr`, `feat`, `fx`, `mark`, `posedraw` — each need the
  scaffold's treatment individually, and the native pass's own composite besides; and the UI
  layer's Vulkan record is emitted
  conditionally on GL twin bookkeeping (`twin_make` / `twin_find` gate `mir_op`), so its record
  cannot be produced at all until the twin table is separated from its GL objects.
* **`ss=1` only**, as everywhere in this plan: `ss=2` has no target on the Vulkan side until 4c.
* **The fps readout needs `mark.on` at launch for the font** on this lane exactly as on the GL one
  — the font reaches the render thread in the frame packet, published at hook 8, which is
  `markown`'s. Under `renderer=vulkan` that hook is installed at DLL attach as always, so the
  requirement is unchanged and not a property of the new backend.

### 2.50 The world draws with no GL context — landing 4b-2 of the Vulkan-only plan

**What this part is.** 4b-1 taught two passes to gather without drawing and fixed the A/B's arming
(§2.49). This part does the other three of the driver's four entry points' worth of work: all five
world passes — terrain, features, effects, markers and units — now run their gather on
`renderer=vulkan`, hand over, and are drawn by their Vulkan twins. `tagpu_overlay.c`'s `gl_draws`
guards one pass now, `tagpu_gui_present`, which is 4b-3.

| pass | two-lane (route D) | vulkan-only (route E) |
|---|---|---|
| terrain | **0 px**, 630 719 ink px | **0 px**, byte-identical (same md5) |
| features | **0 px**, 132 274 ink px | **0 px**, byte-identical |
| effects | **0 px**, 3 327 ink px | draws; cross-run comparison not available |
| markers | **0 px**, 297 ink px | **0 px**, byte-identical |
| units | **0 px**, 2 125 ink px | **0 px**, byte-identical |

One build, `ss=1`, one `.ab` at a time. Terrain and features on `feat-forest` at 1024×768;
markers and units on `selbox-facings`; effects on `fx-lasers`. The effects pass draws transient
projectiles, so two runs agree only if the same ones are alive — its oracle is the same-frame
two-lane A/B, exactly as the frame-rate readout's is, and for the same reason.

#### The shape this landing is made of

**Eleven times, a pass was keyed on GL rather than on what GL stands for**, and each one presented
as a different symptom. They are worth listing together, because the list is the finding:

| where | the test | what it should have asked |
|---|---|---|
| `tagpu_gaf_atlas_create` | `if (a->tex) return 1` | is the atlas LAID OUT (now `a->made`) |
| `tagpu_posedraw.c`'s `unit_ok` | `!m->vao` | does the material name this geometry and agree about the vertex count |
| `tagpu_mark.c`'s `upload_layer` | `glGenTextures(…); if (!s_tex[i]) return 0` | are the layer's BYTES there (the hand-over carries them) |
| `tagpu_mark_render` | `if (textTex)` | is there text to record |
| `tagpu_terr.c`'s `ensure_atlas` | the upload gated the mirror | the mirror IS the buffer the upload was given |
| …and the SAME function's already-built test | `if (s_atlasTex && …)` | is the atlas BUILT (now `s_atlasBuilt`) |
| `tagpu_terr.c`'s gather | `s_maxTex` from `GL_MAX_TEXTURE_SIZE` | the bound of the device that will SAMPLE it |
| `tagpu_render3do.c` | the LUT built only by `_texref` | the LUT is the pass's; the texture is the backend's |
| …and four more of the same two forms | | |

Two forms, then: **a GL handle used as a validity test**, and **a construction reachable only
through one**. Both are invisible while one backend exists, because the handle is always there.

**The counterpart rule that came out of it**: a device limit belongs to the device that will
consume it. `tagpu_vk_max_image_dim()` and `tagpu_vk_max_storage_range()` read
`maxImageDimension2D` and `maxStorageBufferRange` from the physical device actually bound (the
storage range capped at `INT_MAX`, since a driver may report `UINT32_MAX`; §2.94), and
both return **0 for "no device yet" rather than a default** — the lane takes ~200 ms to come up
while the gathers run from the first frame, so a pass that read 0 as a bound would cache a ruined
atlas for the life of the process. That is measured, not hypothetical: it is what
`terr: atlas built 2176x0 … 0 kept` was.

#### Where each pass's line falls

Not at the pass. At the point where the pass stops being API-independent, and that point is
different in each:

* **terrain** — GL in FIVE places outside its draw, four of which the first reading missed:
  `tagpu_terr_gather`'s own `init_gl`, `ensure_atlas`'s upload, `build_height`'s, and
  `build_hills`' VAO. Each gated around the GL and never around the mirror beside it.
* **features and effects** — the simplest: GL in `init_gl`, the render, and the shader helper. But
  `init_gl`'s TAIL held the atlas's `dim`/`max`/`ents`, so gating it left the atlas unsized;
  extracted as `atlas_setup()`, called from the gather on the other lane.
* **markers** — the record and the draw are two PARALLEL statement streams: `mk_push` and
  `mk_draw` are pure CPU at eight sites with the GL interleaved among them, so this one takes two
  block gates and a gate per GL statement, never around a record append.
* **units** — 150 GL calls in `tagpu_posedraw.c` and **none of them needed a gate**: every
  `tagpu_posedraw_*` entry point is called from the composite the vulkan lane exits before. What
  was missing was the RECORD, not a gate. Three body-path functions (`pd_begin`,
  `tagpu_posedraw_unit`, `tagpu_posedraw_end`) gate their own GL in five contiguous runs, and the
  lane calls them from its own exit.

**And the exit itself moved.** It sat at the top of the composite, which meant the lane never
reached the unit pass's gather — `pdu[]` and the vertex emission, three hundred lines below it.
Audited before moving: there is not one GL call between the four world gathers and the "nothing to
draw" return. The pose view moved with it, into the `s_pv` static that already existed, so both
lanes read one construction rather than two.

#### What is NOT covered by 4b-2

* **`tagpu_gui_present` — the UI layer.** Still gated whole. Its record is built inside the GL
  drain (`twin_make`/`twin_find` gate `mir_op`) and, worse, the record's COLOUR flag is keyed on
  the GL colour twin (`(restored && t->rgb)` reaching `TAGPU_GUICOL_ON`), so a missing handle
  would change the record rather than only the draw. That is 4b-3.
* **`ss=2`** has no target on the Vulkan side; that is 4c, with TA's own surface.
* **The composite** — the world FBO, the cast-shadow map, the shadow and slant redraws, the nano
  wire pass, the key/fill inversion and the resolve — has no counterpart on this lane. Each pass
  draws itself into the swapchain from its own hand-over instead, which is why `tagpu_zoom_publish_view`
  does not run there and the input path stays 1:1.
  > **Superseded 2026-09-22 (§2.3a).** The second half of that sentence was true when it was
  > written and then outlived its reason: the clean cut stopped compositing TA's frame, so
  > `keyOn`'s question has nothing left to ask, and the publish is at the foot of
  > `tagpu_native_frame`'s hand-over now. Left un-run, it cost the shipped configuration its
  > scroll wheel, the widened addressable rect, the zoomed minimap box and the widened camera
  > range.
* **The 300-frame composite stats line** does not run on this lane. The lane has its own instead.
* **Classic++'s restorer never starts on the vulkan-only lane, and nothing says so.**
  `tagpu_terr.c`'s `restore_step` returns on `!s_atlasTex` and `tagpu_gaf.c`'s
  `tagpu_gaf_atlas_restore` on `!a->tex`, both 0 with no GL — and the *published Vulkan* restore
  request is fed from inside those, so the Vulkan lane's restore does not start either. With
  `assets=` on, terrain and every GAF atlas would draw unrestored. **The 0 px figures above were
  all taken with Classic++ OFF**, so they are valid for that configuration and say nothing about
  this one. Two more instances of the same shape, left rather than fixed: the GL restore genuinely
  needs its texture, and giving the Vulkan lane its own restore path is its own piece of work.
* **The engine keeps its own unit rasterise on this lane, and that is deliberate.**
  `tagpu_posedraw_live()` is a PROMISE to the game thread — it acts on a yes by skipping the
  engine's own draw and wiping its composite — and the invariant `tagpu_owndraw.c` states is that a
  stale read may only be stale in the direction of *not* skipping, because "skipping when nothing
  will draw has no write order that produces it". Arming the pass without a GL program produced
  exactly that order: the twin stands down whenever a mirror is missing or the lane is not READY,
  and after the retry budget `render_vk.c` degrades to GDI while `tagpu_vk_owns_present()` stays
  latched — so the game thread would skip and wipe for the life of the process and every covered
  unit would be invisible. `live()` therefore answers **no** on this lane, by construction rather
  than by checking whether the twin happened to draw. The engine does work whose output this lane
  does not present; the redundancy goes when 4c gives the backend TA's surface.
  [Found by this landing's cross-thread review.]

#### The instruments, and what they cost to learn

Four periodic reports now exist, all keyed on the driver's frame so they can be lined up:
`native: vulkan lane handed over frame N: …`, `vk: census: frame N: N drew, M claimed …`,
`posedraw: nothing to hand over for frame N - mirrorWant= win= recording= pubHave=`, and
`vk: unit: frame N: the hand-over carries nunit= …`.

They exist because **one-shot latches lied by omission three times in a row.** A latch reports the
first occurrence and is then silent, so one spent on an ordinary case — the shell, where no posed
unit exists and nothing to publish is normal — hides every frame that matters. Two of them in
sequence sent this landing to the wrong function twice, and a third pair, keyed on two DIFFERENT
counters, produced a flat contradiction (`posed=3` and `win=0`, apparently of one frame). The rule
that came out of it: **prefer a periodic line reporting current state, and never spend a latch on
a case that is ordinary.**

### 2.51 The UI layer draws with no GL context — landing 4b-3 of the Vulkan-only plan

**What this part is.** The last of `tagpu_overlay.c`'s four entry points. `tagpu_gui_present` now
gates its own GL objects, its own draws and its own state restore, gathers the drain's record on
either lane, and hands the Vulkan twin a composite to run — so the `gl_draws` variable in
`tagpu_overlay.c` is gone and all four passes are called unconditionally. Landing 4b is complete.

| fixture | comparison | result |
|---|---|---|
| shell (static) | two-lane route D, GL vs Vulkan, same frame | **0 px** of 307 200 |
| shell | vulkan-only vs route D's Vulkan half | **0 px** of 307 200 |
| shell | vulkan-only vs route D's GL half | **0 px** of 307 200 |
| `feat-forest` in game | UI chrome, outside the viewport | **0 px** of 155 648 |
| `feat-forest` in game | inside the viewport | 315 071 of 630 784 |

One build, one `.ab` at a time, `native.on` off so a single pass draws into the Vulkan frame.
307 200 non-black px on all three shell captures.

**The viewport half of the in-game row is not a defect and cannot be closed by this landing.**
With `native.on` off the engine rasterises the world into its own primary and the UI layer
composites whatever moment it finds there, so two separate runs are two separate moments. The
chrome — the panel, the top bar, the side bar, everything this pass actually draws — is what the
`0 px of 155 648` measures. The shell rows are the whole-frame test, and they are the ones that
say the twin store, the sprite path, the string path and the copy path agree byte for byte.

#### The same shape again, and a second one beside it

§2.50 found eleven instances of *a pass keyed on GL rather than on what GL stands for*. The UI
layer carried seven more, and the module's own comments had already written the right answer down
in four of them:

| where | the test | what it should have asked |
|---|---|---|
| `init_gl` | the UI atlas armed at its tail, reachable only through the GL bring-up | the atlas is a CPU table (now `atlas_setup()`, idempotent on `s_atlas.made`) |
| `tagpu_gui_cursor_frame` | `s_gl != 1` | is the atlas armed — *its own comment says "ownership is latched on the ATLAS"* |
| `twin_string` | `!s_strProg` → re-seed the whole surface | is there a string to stamp |
| …and the same function | `tagpu_text_glyph_tex()`, a GL uploader | has the cache rasterised anything (now `tagpu_text_glyph_have()`, that uploader's own first line) |
| `mir_finish` | `if (f->surface_tex)` gating the copy of TA's primary | does this frame carry TA's surface — *the bytes are `g_ddraw.primary->surface`, CPU memory the texture has nothing to do with* |
| `sharp_minimap`'s bake | `!s_mmTex` | is this palette's resolve of this map in `s_mmPicRgb` (now `s_mmBaked`) |
| `twin_make` | the entry and its texture made together | the TABLE says a twin exists; `tex`/`fbo` at 0 is a twin with no GL |

**And a second shape, which is the one this landing nearly shipped.** Twenty-one functions in
`tagpu_gui_surf.c` contain a `gl*`/`x_gl*` call, and on `renderer=vulkan` they are inert for
**three different reasons** — which matters, because a reader auditing "the head-returns" will
find seven and will not know what protects the other five:

| how it is inert on route E | count | which |
|---|---|---|
| returns at the head | 7 | `twin_colour`, `twin_col_drop`, `twin_upload`, `twin_clear`, `restore_step`, `upload_palette`, `unbind_all` |
| unreachable — the CALLER gates | 3 | `mksh`, `mkprog`, `init_gl` (76 calls, all behind `tagpu_gui_present`'s one test) |
| its GL calls are keyed on handles that are 0 | 2 | `twin_drop`, `sharp_drop` — `twin_make` never fills `tex`/`fbo` and `twin_colour` never fills `rgb`, so every `if (t->…)` is false |
| gates inside itself | 9 | the four record-bearing ones (`twin_make`, `twin_sprite`, `twin_string`, `twin_copy`), the four sharp-layer/composite ones (`draw_layer`, `sharp_begin`, `sharp_cursor`, `sharp_minimap`), and `tagpu_gui_present` |

[The count and the three mechanisms were corrected by the 4b-3 landing review; the first draft of
this section said "twelve return at the head", which was the total of the first two rows plus
guesswork.]

Three of that last row look exactly like the head-return seven — no return value, nothing but
`gl*` calls to a first reading — and are not:

| function | what a head-return would have dropped |
|---|---|
| `draw_layer` | `s_mHand`, the hand-over that TELLS the Vulkan twin what composite to run. The twin would have had no UI at all. |
| `sharp_begin` | `s_sharpOn`/`s_sharpInk`, and the two clients whose `mir_sdraw` records are the twin's only copy of the sharp layer |
| `sharp_minimap` | the CPU bake of the minimap through the presented palette — `s_mmPicRgb`, handed over as `mmPic` and uploaded by `tagpu_vk_gui.c` into an image of its own |

All three were written as head-returns first and caught by reading the file for `mir_*` and
`s_mHand` writes rather than for GL calls. **The audit that finds the first shape does not find
this one**: grepping for `gl[A-Z]` tells you which lines are GL, never which of the remaining
lines somebody else is waiting for. The question to ask of every candidate head-return is *what
does this function publish*, and the two files to grep are the record's and the hand-over's.

`tagpu_posedraw_live()` in 4b-2 was the first of these; this landing found three more in one file.

#### The A/B, and two instruments

**The UI pass claimed its Vulkan half on `wrote`** — the GL capture reaching the disk — which is
precisely the trap 4b-1 moved the five world passes off, left standing in the sixth. It now claims
through `tagpu_vk_ab_arm("gui")` (already in `s_abFiles`, so no table change), and the GL lane
needs both its own write and that unlink.

**The heartbeat reported the engine's minimap pair as the GL texture's texel count**
(`s_mmEngW * s_mmEngH`), so it read `fog=13227/13356` on the GL lane and `fog=13227/0` on the
vulkan one — *"no engine pair"* about a pair it had just counted 13 227 fogged texels in. It reads
the size from the packet now, which is where both lanes get it. Same rule as §2.50's one-shot
latches: an instrument that is only true on one lane is worse than no instrument, because it
sends the next session to the wrong function.

#### What is NOT covered by landing 4b-3

- **Classic++'s restorer does not start on this lane** — `cpp=0 assets=0 colvalid=0 rgb=0` on both
  lanes in every run here, so the colour-twin path (`TAGPU_GUICOL_DST`/`ON`, `twin_colour`,
  `restore_step`) is honestly "indexed" on both and was never exercised. Both lanes agree because
  neither has colour, not because the colour path was measured.
- **`sharptest`'s quads** were not captured, and the minimap was exercised only with `mmbase`
  armed: at k = 1 the engine's own minimap stands by design (§13.6), so the default path through
  `sharp_minimap` returns early on both lanes.
- **The hand-over still copies TA's primary every frame** into `s_mHand.eng` — that is what makes
  the twin composite at all today, and landing 4c is where the backend uploads TA's surface
  instead.
- The in-game viewport, as above.
- **`s_curDrew` was weaker on route E than on the GL lane — CLOSED by gate 4's last item, §2.57.**
  It is what lets the engine's own cursor blit stand down, and on the GL lane `draw_layer` sets it
  after the composite has actually run. On route E there is no composite there: the condition is
  that the mirror RECORD survived to `draw_layer`'s tail, while `tagpu_vk_gui_prepare` and
  `tagpu_vk_gui_record` can still refuse the frame afterwards. `tagpu_cursown_publish` was called
  from `render_vk.c` **before** `tagpu_vk_frame` in the same iteration, so nothing at that point
  could know the outcome, and the engine's cursor was suppressed on frames that composited nothing.
  **Closed by publishing after the frame instead**, as `cur_drew && tagpu_vk_ui_composited()`, with
  the disagreeing frames counted as `held=`. [The gap was named here by the 4b-3 landing review;
  read §2.57 for how it was closed and what is still not covered.]

### 2.52 TA's own frame reaches the Vulkan lane — landing 4c-1 of the Vulkan-only plan

**What this part is.** The frame's BOTTOM layer. Everything of ours is drawn over TA's own 8-bit
screen, and what we do not draw is what the player still sees. On the GL lane the fork does that
itself — `render_ogl.c:260` builds `g_ogl.main_program` from `PASSTHROUGH_VERT_SHADER` +
`PALETTE_FRAG_SHADER` and draws it before any pass of ours runs, which is why `tagpu_gui.off`
still shows a game there. On `renderer=vulkan` it did not exist: the seam cleared to the lever's
colour and TA's surface reached the frame **only** through the GUI pass's mirror hand-over. That
is one of the three blind spots landing 1 named, and it is closed.

| fixture | before | after |
|---|---|---|
| shell, `renderer=vulkan`, `tagpu_gui.off` | the lever's flat clear | the shell, complete |
| `feat-forest` in game, same levers | the lever's flat clear | the game, complete — minimap, bars, text, terrain, units, cursor |
| in game, census | — | `unit=1`, 8 units posed: our unit pass still draws OVER the new base layer |
| shell A/B, both configurations | 0 px of 307 200 | **0 px of 307 200**, unchanged |
| GL lane in game | — | clean and pictured, 0 faults |

#### Three pieces, and where each had to live

- **`tagpu_surf.c`** owns the snapshot: one per frame, taken from `tagpu_overlay_draw` after
  `tagpu_pal_frame` and before any pass gathers, so every consumer in the frame sees the same
  bytes *and* the same palette. It moved out of `tagpu_gui_surf.c`'s `mir_finish`, which had
  taken it for `s_mHand.eng` — that made the Vulkan lane's bottom layer exist only while the UI
  pass ran, which is the coupling this landing undoes. `mir_finish` now reads it.
- **`tagpu_vk_surf.c`** draws it, running the same GLSL as the GL lane.
- **The seam** calls `prepare` before the render pass and `record` first inside it. The
  `vkCmdClearColorImage` stays: it is what covers the letterbox and any frame this refuses.

**It is not an engine read**, so nothing joined `thread-split.allow`: `g_ddraw.primary` is the
FORK's DirectDraw object, not the game's memory at an absolute address — the standing
`tagpu_gui_surf.c` already read it under. The lifetime argument is the critical section's and
unchanged: `IDirectDrawSurface.c` NULLs the primary inside `g_ddraw.cs` and frees after,
`dds_Flip` swaps `->surface` inside the same section, and the bound comes from the PRIMARY's own
geometry rather than `g_ddraw.width/height`, which is the device mode.

#### The orientation, and two wrong derivations before the right one

The conclusion — **this pass takes no viewport flip** — was reached twice by reasoning that did
not hold, and both are written down because the second one nearly shipped as a rule for the next
landing.

**The GL lane builds TWO quads for this blit, and they differ by exactly a y negation:**

| quad | where | tex `(0,0)` at |
|---|---|---|
| the WINDOW quad | `render_ogl.c:576-597`, the `else` | clip `y = +1` |
| the FBO quad | `render_ogl.c:554-575`, under `if (g_ogl.shader1_program)` | clip `y = -1` |

The first is what the GL lane presents. The second feeds an offscreen target that a later pass
turns back over, and it is reached only when a custom upscaling shader is configured.

**Attempt 1 — inherit the flip.** The pass took the negative-height viewport every other ported
pass takes, on the argument that a ported shader fed the GL lane's own vertices reproduces the GL
lane's image. It drew TA's shell upside down.

**Attempt 2 — a rule about literal quads.** The flip was removed, which fixed the picture, and
the reason was written as: the other passes COMPUTE a clip position from GL-convention uniforms so
the flip corrects their arithmetic, while this one is handed a literal quad whose texcoords move
with it, so flipping only mirrors. That sounds right and is not a rule. `build()`'s `quad[]` is
the **FBO** quad — the paper cited `:557` without noticing it sat inside the `shader1_program`
branch — and Vulkan's y-down clip space undoes its negation, which is the whole of why the
picture is upright. **The window quad WITH the negative-height flip is equally correct.**

**What is actually true: the quad and the flip are ONE choice and must be made together.** Either
pairing works; what does not work is copying one lane's quad and the other lane's flip decision.
A reader who takes `render_ogl.c:580-593` — the quad the GL lane actually presents — and drops the
flip on attempt 2's authority gets the upside-down picture back.

**Consequence for 4c-2, whose resolve is the next literal-quad pass.** `tagpu_native::DVS` is
`uv = p; gl_Position = vec4(p.x*2.0-1.0, p.y*2.0-1.0, 0.0, 1.0)` over a unit-square attribute, so
its pairing is `uv = 0` at clip `y = -1` — the FBO quad's pairing, not the window quad's. Under
Vulkan's y-down that puts `uv = 0` at the TOP of the viewport.

**SETTLED 2026-09-18: that resolve takes no flip, and the argument does not need to know which way
up either image is.** Under a POSITIVE viewport height, `DVS` maps source row 0 to destination row
0 under *both* APIs — GL sends clip `y = -1` to framebuffer row 0 and samples `uv.y = 0` from texel
row 0; Vulkan does the same. It is an orientation-PRESERVING copy either way, so the only thing
left to check is that the two images agree, and they do: the world passes write the engine's
downward-growing screen y under a positive viewport, so the game's top row lands on row 0 of the
`ss×` offscreen image for exactly the reason it lands on row 0 of the swapchain image today
(landing 5b). A negative height here would be the third turn, and would mirror.

**And what kept this looking open was three comments contradicting their own code.** `af82f45`
turned the five world viewports positive and rewrote the prose in the five pass headers,
`tagpu_vk_pass.h`, four places in this file and three cull comments — but left the INLINE comment
at the `vkCmdSetViewport` call site in `tagpu_vk_terr.c`, `tagpu_vk_fx.c` and `tagpu_vk_feat.c`
reading *“y starts at the bottom and the height is negative, so clip space is turned over once”*,
sitting directly above `vp.y = 0.0f; vp.height = (float)h;`. `_unit.c` and `_mark.c` got the
corrected wording; those three did not — and they are the first files anyone porting the world
target opens. Corrected 2026-09-18. **The lesson is narrower than “update your comments”: a
landing that corrects a fact has to grep for the fact, not for the files it already knows it
edited.** Five headers were found by walking the pass list; the call sites were not on that list.

[Attempt 2 was the committed text for two commits, including a sharpened version that made it a
one-line test. The 4c-1 landing review disproved it against `render_ogl.c`'s two branches.]

#### The shader came from the fork, which cost four changes to the generator

`tools/spirv-gen.py`'s rule is that the GL lane's GLSL is the one copy and the Vulkan edition is
generated from it. Honouring that for a shader in the fork's own `inc/openglshader.h` needed:

| change | why |
|---|---|
| `HEADER_SOURCES` | the source is `inc/openglshader.h`, not `src/<name>.c`. Same shape as the already-special-cased `RESTORE_HDR`. |
| `_DECL` widened | it matched only `static const char* NAME =`; the fork writes `static char NAME[] =`, so 11 literals were counted and 0 extracted. |
| `ATTR_LOCATIONS` | the fork asks the LINKER where its attributes landed (`glGetAttribLocation`, `render_ogl.c:549`), so there is no `layout(location=)` and none derivable. In Vulkan a location is pipeline state, so whoever builds the pipeline chooses — the choice is made in the manifest beside `PROGRAMS`, and the pass binds to match. |
| `NOT_PROGRAMS` | "a shader no program uses" guards OUR two lanes from drifting while one is unwritten; it should not force Vulkan editions of the fork's nine other shaders. The tool's own `die()` invited this: *"add it, or say here why it is not a program"*. |

And one latent bug the new source exposed: `--check` tested `line.startswith("/* tagpu_")`, so a
source not named that way had every hash attributed to no key and reported *"has no `words` hash
— regenerate"* however freshly it had been generated.

**Blast radius, verified rather than assumed:** exactly one line changed in each of the twelve
existing headers — the transform's identity hash, which moves because the tool was edited. **Not
one SPIR-V word moved.**

#### What is NOT covered by landing 4c-1

- **`ss` is still 1× on this lane.** There is no offscreen world target and no resolve; that is
  4c-2, and its shader is already generated (`native_d`, the 2× → 1× downsample).
- **The GUI twin still uploads its own copy** of the same bytes from the hand-over, so on a lane
  with the UI pass on, TA's surface is uploaded twice and drawn twice. Harmless — the UI layer
  covers the base — but it is redundant and belongs to whichever landing makes the twin read the
  backend's image.
- **`s_curDrew`'s ordering**, carried forward from §2.51 and still open.
- The in-game picture above has `terr`/`feat` unarmed, so those layers come from TA's surface
  rather than from our passes. That is the fixture, not a limit of the landing.

### 2.53 The world gets a target of its own — landing 4c-2 of the Vulkan-only plan

**LANDED 2026-09-18.** The five world passes no longer draw into the swapchain image. They draw
into an offscreen colour+depth pair at the GAME's resolution times `ss`, and one LINEAR draw puts
that block on the frame at the letterboxed viewport rect — which is what `tagpu_native.c` has
always done on the GL side. `tagpu_vk_world.c` owns the target, its render pass and the composite.

#### It was not a quality lever. Two passes were standing down on the shipped default

`ss` is **2** unless `tagpu_ss.off` is there (`tagpu_native.c`: `s_ss` is set from the file's
ABSENCE), the GL twin calls `glLineWidth(ss)` so a line one game pixel wide is `ss` pixels of the
supersampled FBO, and the Vulkan lane had no such target — so a 2-px line had nowhere correct to
go and `tagpu_vk_fx.c` and `tagpu_vk_mark.c` refused the **whole pass** on any frame carrying line
vertices. Measured on the lane before the landing, in a live game on `renderer=vulkan` with
`fx.on` and the `fx-lasers` fixture:

```
vk: fx: ss=2 makes the GL twin's lines 2 px wide and this device's pipelines are built
        at 1.0 (wideLines is not enabled) - nothing drawn while there are line vertices
```

The census read `fx=1` on frames without lasers and the pass dropped every frame with one. **That
is what "`ss=2` has no target on the Vulkan side" actually cost**, and reading the source would not
have said it: both refusals look like device-capability guards, and the connection to `ss` being 2
by default is three files away.

So the landing also asks the device for **`wideLines`** — a CORE feature bit, so it sits beside
`samplerAnisotropy` in `pEnabledFeatures` and is *not* on the extension retry ladder — publishes
`wideok` / `maxLineWidth` on `TAGPU_VKPASS`, gives fx's line pipeline a dynamic width, and turns
both refusals from `ss != 1` into a range check. **Refused, never clamped**: a clamped width is a
line a different thickness from its own oracle, which is what that family of flags exists to stop.

#### No pass file moved a pipeline, and that is by construction

Vulkan render-pass **compatibility** compares attachment formats and sample counts and ignores
load/store ops and layouts. An offscreen pass built with `d->fmt` and `d->dfmt` in the seam's own
order therefore accepts every pipeline already built against `d->rp`. What the passes needed was
the **extent**, and they were already written for it: `terr_scissor` computes `sx = w / uGame.x`,
so handing it `gw * ss` yields exactly GL's `glScissor(vpL*ss, vpT*ss, vw*ss, vh*ss)`, and the
vertex shaders divide by `uGame` rather than by the target. `tagpu_vk_pass.h`'s promise that a
pass "could draw into an offscreen image, a swapchain image or another process's image without a
line of it changing" was this landing's to keep, and it kept — the five world passes are
unchanged apart from fx's line width.

#### The orientation, settled without naming which way up anything is

`tagpu_native::DVS` pairs `uv = 0` with clip `y = -1`. Under a **positive** viewport height that
maps source row 0 to destination row 0 under *both* APIs, so the copy is orientation-preserving
and the only question left is whether the two images agree — and they do, because the world passes
write the engine's downward-growing screen y under a positive height (landing 5b). `tagpu_native.c`
states the same fact from the GL side at the FBO bind: *"in this FBO window y == game frame py, so
the rect maps directly"*. §2.52 has the three re-derivations this replaces.

#### The `ss` decision is published, not recomputed

`tagpu_native.c` settles `ss`/`devres` once above the effects gather — the paragraph there says
why, and the first cut of `devres` put the scaffold test 1.5x out by raising `ss` too late. It now
publishes that decision as `TAGPU_WORLDTGT` (`tagpu_native_worldtgt`) from **above** its own
`!gl_draws` return, so it is this frame's on both lanes and the backend sizes its target from the
same number rather than deriving `s_ss ? 2 : 1` for itself.

#### What was measured

| | |
|---|---|
| terrain A/B, route D, `ss=1` | **0 px of the 630 719 the GL FBO drew** |
| the other 118 751 differing px | all pixels GL leaves **black** — see below |
| live, `ss=2` | `vk: world: 2048x1536 target (1024x768 at ss=2) -> (0,0 1024x768), 57 697 frame(s), 8 build(s)` |
| the fx stand-down | gone; `wideLines is not offered` never logged, so the device gave it |
| the frame on screen | 4 901 distinct colours against 103 before — the supersample resolving |

#### 4c-1 SILENTLY BROKE EVERY WORLD PASS'S A/B, and 4c-2's review is what found it

The terrain A/B on the first cut of this landing read **NOT identical, 118 751 px of 786 432** —
and 0 of the 630 719 the GL capture drew. The cause was not 4c-2. **The two halves had stopped
being the same kind of picture when 4c-1 landed the day before:** the GL half is the bare world
FBO, black wherever the pass did not draw, and the Vulkan half is the **swapchain image**, which
since §2.52 carries TA's own 8-bit frame underneath. 4c-1 verified itself on the screen rather
than through a world A/B, so nothing caught it.

**For a pass that BLENDS it was worse than cosmetic.** The effects pass's Vulkan half is *the
effect over TA's frame* and its GL half is *the effect over black*, so every translucent fragment
differed too. Measured, of its 1 622 GL-ink pixels: 226 identical with a median max-channel of
**237** (the opaque fragments, byte-for-byte) and 1 396 differing with a median of **19**. The pass
was correct; the harness could not say so.

**THE FIX IS ONE LINE IN THE SEAM, and it came out of the landing review.** `tagpu_vk.c`:

```c
if (draw_surf && nclaim == 0)          /* not on a frame an A/B has claimed */
    tagpu_vk_surf_record(...);
```

`nclaim` is this frame's armed A/B levers, already counted before the render pass begins, so TA's
frame is withheld on exactly the frames a measurement is being taken on and on no others. It is
the same kind of measurement-only divergence as `tagpu_vk.on`'s `color=0,0,0`, which exists so the
two clears agree. **Both world A/Bs are 0 px again:**

| pass | fixture | result |
|---|---|---|
| terrain | `feat-forest` | **0 px of 786 432**, 630 719 non-black a side |
| effects | `fx-lasers` | **0 px of 786 432**, 31 532 non-black a side |

**`vk-ab.py` kept the diagnostic and lost the escape.** It still prints `on GL ink` / `on GL black`,
because that split is what tells an over-draw from a framing difference — but it no longer **exits
0** when every difference falls on a GL-black pixel, which the first cut of it did. That escape was
blind to the one failure this project has already shipped once: G19e's line rasterisation drew
*"all 126 of the twin's pixels plus exactly one extra fragment at the end of each line segment"*
(`tagpu_vk_pass.h`), and every one of those extra fragments lands on a pixel GL left black. An
over-draw would have exited 0 under the words *"the pass agrees with its twin"*. **The split is a
thing to read, not a verdict** — and the docstring that said *"the exit status is 0 only when every
pixel agrees"* is true again.

**The fuller fix is still filed**: capture the Vulkan half from the offscreen world target itself,
which is cleared to `{0,0,0,0}` and holds the world alone. That is what would make a world A/B
runnable at `ss > 1`, which it has never been — the GL capture is the supersampled FBO and the
Vulkan one is the client rect, so the two are different sizes and `vk-ab.py` refuses them.

#### One bug the live run caught and reading did not

`_down_owed` is a **request** that the seam tear a module down, not "there is something to free".
Set on the success path it made the seam destroy and rebuild the entire target every frame behind a
`vkDeviceWaitIdle` — **40 builds in one minute of play**, and the picture was still correct, which
is exactly why no screenshot would have found it. It is now set only on the two refusal paths
beside `ST_REFUSED`, which is `tagpu_vk_surf.c`'s shape. The heartbeat that made it visible
(`N frame(s), M build(s)`) was added for this and stays.

#### What the landing review found, beyond the A/B

Eight findings, all acted on. Three are worth carrying past this landing:

- **A pass that scales something by `ss` must ask the TARGET, not its hand-over.** The effects and
  marker passes set their line width from `h.ss` — what the GL lane did — while whether there IS
  an `ss` target this frame is `tagpu_vk_world_prepare`'s answer, and on its refusal paths the seam
  records the same passes into the swapchain image at 1:1. That would have drawn `ss`-wide lines
  over a 1:1 world for the rest of a session, silently: the refusal this landing removed, with the
  sign flipped. The target is now built **first of all `prepare`s** and the passes read
  `tagpu_vk_world_scale()`. The general shape: *the code asserted an invariant it never
  established, and the comment stated the premise as fact.*
- **A subpass dependency's SOURCE scope is every way the image was last used, not the interesting
  one.** `dep[0]` named only `FRAGMENT_SHADER`/`SHADER_READ`, so this frame's `LOAD_OP_CLEAR` was a
  write-after-write against the previous frame's colour and depth writes with no availability
  operation. `tagpu_vk.c`'s own render pass carries exactly those masks and states the argument in
  terms; the fence wait is what made it work, which is safety by timing.
- **Published state that nothing reads is worse than absent state.** `TAGPU_WORLDTGT.serial` was
  documented as letting a consumer tell "the same target" from "a new one" without comparing
  fields — and no consumer ever did. It is gone. `devres` stayed and is now *reported*, because it
  changes which of GL's two shapes the composite is reproducing.

And one that is simply embarrassing and worth naming as a habit: **five comments this landing
falsified were left standing, two of them directly above the code that contradicts them** —
including a whole block in `tagpu_vk_fx.c` that a search-and-replace inserted a correction *beneath*
rather than over. This is the same defect the landing's own first commit fixed in three other
files (§2.52). A landing that corrects a fact has to grep for the fact.

#### Not covered by 4c-2

- **`selAt1x`.** GL draws the selection rects at 1x over the resolved frame, occluded by a
  `GL_NEAREST` depth blit down from the `ss` buffer. This lane rasterises them at `ss` and
  downsamples with the rest — which is GL's own `selgeom main` configuration, not an invention, but
  it is a softer rect. Porting it needs a second 1x image, a depth resolve and the marker pass
  recorded twice; the offscreen depth attachment is `STORE_OP_DONT_CARE` until then and the code
  says so.
- **The two-step resolve.** GL resolves `ss -> 1x` and then composites 1x -> viewport with
  `GL_NEAREST`; this draws the `ss` image straight into the viewport with `LINEAR`. At k = 1 those
  are arithmetically the same filter (a destination centre lands on the corner of a 2x2 source
  block, so bilinear IS the box filter — which is what GL's own `GL_LINEAR` on `s_colTex2` is for).
  At k != 1 they differ, and this lane's behaviour is GL's `devres` path.

### 2.54 The oracle moves to the world target, and finds the GL half wrong — landing 4c-3

**LANDED 2026-09-18.** A world A/B now compares two `gw*ss` by `gh*ss` pictures, so it runs at the
supersample factor the renderer actually ships with. It never has before.

#### The two halves were never the same rect, and only one of them said so

The GL half of a world capture is the **world FBO**: `tagpu_native.c` binds it and sets
`glViewport(0, 0, gw*ss, gh*ss)`, and `tagpu_abshot.c` reads *the viewport* back. The Vulkan half
was `s_vk.img[idx]` — the swapchain image, at the window's client rect. Those coincide only when
`ss == 1` **and** nothing is letterboxed. `ss` is 2 unless `tagpu_ss.off` is there, so the shipped
configuration was never measurable, and every 0-px result on this plan was taken at `ss = 1`.

Four passes (`terr`, `feat`, `fx`, `mark`) knew and refused themselves — *"the A/B needs ss=1 (the
GL capture is the supersampled FBO)"*. **`posedraw` did not**, and wrote a mismatched pair that
`tools/vk-ab.py` then refused by size, one step further from the cause.

§2.53 built a `gw*ss` by `gh*ss` colour image holding the world alone over a transparent clear,
which is exactly what the GL FBO is. This points the capture at it:

- the world colour image carries `TRANSFER_SRC` — **asked for at creation**, because
  `vkCmdCopyImageToBuffer` requires the usage and no layout transition confers it. The same finding
  was made against the swapchain images in the 2026-09-15 review, where the reference ICD copied
  anyway and a whole 0-px result rested on undefined behaviour;
- `tagpu_vk_world_shot(slot, …)` answers only for the slot the world render pass was actually
  **opened** on this frame. `s_drawThis` cannot serve — `record` clears it, and `record` runs before
  the capture is recorded. Every other slot still holds a real image of an *earlier* frame, which is
  the worst thing an oracle can hand back;
- the seam picks it whenever the claiming row is one of the first five of `s_abFiles` (the world
  rows), out of the same array the path came from, so "which file" and "which image" cannot drift;
- a world claim on a frame with **no** target is refused by name in the lane's own log instead of
  being captured at the wrong size and diagnosed afterwards by the diff tool.

The four `ss != 1` refusals are gone. The bound they were is now a property of the images.

#### Measured at `ss = 2`, route D, 1024x768 game — 2048x1536 a side

| pass | fixture | differing | non-black a side |
|---|---|---|---|
| terrain | `feat-forest` | **0 of 3 145 728** | 2 522 876 |
| features | `feat-forest` + `eye 1400 1600` | **0 of 3 145 728** | 529 096 |
| units (`posedraw`) | `selbox-facings` | **0 of 3 145 728** | 8 465 |
| effects, no line vertices | `fx-lasers` | **0 of 3 145 728** | 10 396 |
| effects, **with lasers** | `fx-lasers` | ~100 of 3 145 728 | 5 460 – 14 256 |
| markers, **with order lines** | `selbox-facings` + held SHIFT + `patrol` | 34 of 3 145 728 | 2 448 |

All five world passes, and the two non-zero rows are the same one thing (below). `ss = 1` was
re-measured on this build for comparison: terrain **0 of 786 432** with 630 719 ink a side, which is
the §2.53 figure unchanged.

#### And the laser difference is the GL half being wrong, which is a first

Every differing pixel is one GL left black; **not one pixel GL drew differs**, and the extra pixels
carry the same colour as the line. Printing the band shows why: every Vulkan column of the line
holds **two** ink pixels and every GL column **one**.

**Measured on the marker pass too, on both axes.** Order lines (`order.on`, SHIFT held, a `patrol`
that never completes) give a vertical segment where GL inks column x = 620 and Vulkan inks 619–620,
and a horizontal one where GL inks row y = 739 and Vulkan inks 739–740 — 34 px, all of them the
line's own white (235, 235, 235), none of them a pixel GL drew. So it is the `glLineWidth(ss)` call
and not anything particular to one pass.

The cause was already in the tree, as a measurement, in `tagpu_native.c:337`: *"the driver clamps
an aliased line's width to 1 — `glLineWidth(ss*3)` draws pixel-identically to `glLineWidth(ss)`"*.
So the GL twin asks for `glLineWidth(2)` in the `ss` buffer and gets 1, and the note says what that
costs: *"a half-lit smear, about half the engine's colour"*. That is the defect **`selgeom` was
invented for** on the selection rects. The Vulkan lane has had `wideLines` since §2.53 and gets the
2 px it asks for.

**The right way to say what that buys is COVERAGE, not "one lit pixel".** At `ss = 2` a game pixel
is a 2×2 block of target pixels, so per game-step along the line GL deposits 2 target pixels — 0.5
game pixels of coverage, exactly the "half-lit smear" — and Vulkan deposits 4, i.e. **1.0**, which
is the engine's own rule (one whole coloured pixel per major-axis step, `0x4BE950`). Whether that
1.0 lands in one game pixel or splits 0.5/0.5 across two depends on where the line falls against
the block grid, so the *energy* is the engine's and the *placement* is not guaranteed to be.

**Left alone deliberately.** Making the two agree means changing a shipped picture, in one
direction or the other, and that is the owner's call rather than a landing's. What changes here is
only that the difference is now visible and explained instead of unmeasurable.

#### The three UI captures, and what they are and are not bound by

They still read the **swapchain** image, because their GL halves are the default framebuffer —
`tagpu_scaffold.c` and `tagpu_fps.c` contain no `glBindFramebuffer` at all, and `tagpu_gui_surf.c`
binds only its own mirror FBO and 0. So they are the reason the seam withholds TA's own frame on a
claimed frame, and the comment on that line no longer claims the world captures depend on it.
Measured on this build: the GUI A/B is **0 px of 307 200** on the shell.

**They were never `ss`-bound, and a first draft of the `ta-drive` note said they were.** `ss` sizes
only the world FBO (`s_fbo2`), which none of the three ever binds; what their halves need to agree
about is the WINDOW's size — no letterbox, `k = 1`. None of them ever carried an `ss != 1` refusal
either; only `terr`, `feat`, `fx` and `mark` did, and this landing is what removed those.
[CAUGHT BY THE 4c-3 LANDING REVIEW.]

#### Not covered by 4c-3

- **`devres` and `k != 1` are untested either way.** Every figure above is a 1024x768 game in a
  1024x768 window, where the composite rect is the whole frame.
- **The steady-state cost of `TRANSFER_SRC` on the world target is not measured.** On several
  drivers — AMD's DCC above all — that usage disables lossless framebuffer compression for the life
  of the image, and this image is written by five passes and sampled by the composite every frame.
  It is an unconditional flag serving a lever that fires on one frame; the code says so where the
  flag is set, and nobody has put a number on it. [RAISED BY THE 4c-3 LANDING REVIEW.]
- **A capture at 1080p with `ss = 2` is 3840x2160**, which is 33.2 MB malloc'd on the render thread
  for the GL half and another 33.2 MB of host-visible coherent memory for the Vulkan one — in a
  32-bit address space whose budget `tagpu_vk_shot.c` calls 8.3 MB *"real money"*. Both failure
  paths refuse and log, so it is a refusal rather than a fault, but the four passes could not reach
  it before this landing. [RAISED BY THE 4c-3 LANDING REVIEW.]

### 2.55 Route D's window goes, and with it the oracle — landing 4d-1

**LANDED 2026-09-18.** 152 lines in, 370 out. The Vulkan lane no longer has a window of its own,
and can no longer run beside the GL backend at all.

#### What went, and why all of it was one thing

Every line of it existed for the reason `tagpu_vk.h` states: *"two backends must not both present
to one window in one frame"*. Route D answered that by giving the lane an **owned popup** over the
game window's client area — `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`, `HTTRANSPARENT` to the mouse,
created/moved/destroyed on the thread that pumps messages (because a window whose messages nobody
dispatches deadlocks anything that sends it one), reached by posting `WM_TAGPU_VK` to the game
window, and kept on the client rect by following the owner's `WM_WINDOWPOSCHANGED`. All of that
is gone: the class, the window proc, `vkw_place`/`vkw_create`/`vkw_request_destroy`/
`vkw_release_unless_worker`, `s_vkwnd`, `s_askedWin`, and the four teardown sites whose whole job
was getting the destroy ORDER right against a live surface.

**And `render_ogl.c`'s own call to `tagpu_vk_frame` went with it, which the plan's 4d entry did not
mention.** That call was the GL backend's half of route D — it drove the Vulkan lane from inside
the GL iteration and skipped `SwapBuffers` when the lane had presented. Removing it is what makes
`s_ownWin` true on every frame that reaches the lane, because `render_vk.c` is now the only driver.
**`tagpu_vk_enum_start()` stays** where it is in `render_ogl.c`: the GPU row is the player-facing
half of Phase G and has to work under the GL backend too.

#### What survives, and it is not tidiness

`tagpu_vk_wndproc` keeps its name and its call from the fork's wndproc, reduced to the single arm
that was never about a window of ours — **the game window being destroyed**. That is the only fact
the window thread holds which the render thread cannot get for itself: `g_ddraw.hwnd` is nulled
only by the IAT-hooked `DestroyWindow` (`winapi_hooks.c`), and only *after* the real destroy, so a
destroy by any other route leaves `hwnd != s_owner` false and the lane presenting on a dead HWND
until the driver raises `VK_ERROR_SURFACE_LOST_KHR`. `s_ownGone` bounds that at one frame. Deleting
the observer with the rest would have traded an ordering for "wait for an error", which is the one
shape this file is not allowed to ship.

`s_ownWin` is also kept deliberately. After this the latch is always true and every
`tagpu_vk_owns_present()` test in the tree is a constant — but that collapse reaches every GL draw
site landing 4b stood down, and the plan scopes 4d to the window. A one-way landing that sprawls is
how something nobody meant to lose gets lost.

#### The cost, which is the point of the landing

Route D **was the oracle**: both backends rendering the same frame, each capturing its own half,
`tools/vk-ab.py` diffing the two. No absolute two-lane comparison is expressible after this, and
every figure that wanted one was banked in 4c-3 first (§2.54) — all five world passes at the
shipped `ss = 2`, the UI layer at 0 px of 307 200, and the `ss = 1` set before them. From here a
claim is **relative**: this build against the previous one.

#### Verified by running it, because there is nothing left to A/B with

On `renderer=vulkan` with the full play arm set, and the clear colour left at its **magenta**
default so that any pixel the lane failed to cover would be unmistakable:

| what | result |
|---|---|
| 640x480, steady state | **one** real window (the game's own, no popup); census 6 passes drawing; world target 1280x960; terrain, trees, rocks, four units with health bars, HUD, minimap, cursor. **0 magenta of 307 200** |
| a **mode change** — the path the window handshake used to live in | shell 640x480 → `render thread stopping - down` → clean re-bring-up at 1024x768 *on the same window*; 5 passes; world target 2048x1536; water, seven units, a solar collector, a nano effect. **0 magenta of 786 432** |
| stop | clean in 0.44 s, nothing left behind |

#### Three instruments the deletion killed, found by its own review

The landing verified its picture and did not ask what else had been reading route D. All three were
**silent** failures, which is the shape this stack's own history is made of.

- **`tagpu_ftime` was inert for the entire session under `renderer=vulkan`.** `tagpu_ftime_poll()`
  — the only thing that sets the module's `s_on` — was called from exactly one place, `render_ogl.c`,
  which after 4d-1 never drives this lane. So no Vulkan timestamps were written (`tagpu_vk.c` gates
  them on `tagpu_ftime_armed()`), every sample was discarded, and not one line reached the log. The
  gate clause it exists for, **"frame time no worse than GL", is still open** — the landing that
  deleted the other lane nearly deleted the only way to measure this one. Fixed in two halves,
  because the first was not enough: the poll now runs from `vk_render_main`, **and** `report()` got
  a caller on this lane (both its call sites were inside the GL bracket, so the samples accumulated
  and nothing printed them). `s_glDrives` makes exactly one lane drive the report, whichever is
  running. Measured after the fix, 1024x768, full arm set: **`vk p50 0.154 ms  p99 0.370 ms`
  (n=256/48838)** — the first frame-time figure this lane has ever produced, and the baseline the
  cross-build comparison starts from. The `vk/gl` ratio is gone with route D and cannot be computed
  in one process again.
- **`tools/uiwalk.py --vk` is permanently broken** — the walk the G19f UI-layer clause was *met*
  with. It armed `vk.on` **without** `renderer=vulkan`, which was route D exactly; that arm set now
  brings up no lane, so `tagpu_gui_vk.ppm` is never written. It failed closed (`Walk.vk_ab` returned
  `None` and refused to score it) but burnt the PPM settle deadline at all 13 stops while doing
  nothing. `--vk` now refuses with a message saying what replaced it.
- **`tagpu_vk.on` under `renderer=openglcore` was NOT inert, and a first draft of this section said
  it was.** `tagpu_vk_armed()` is not on the frame path and still returns 1 from the file, which
  latched six one-way gather mirrors — `tagpu_fx.c` and `tagpu_feat.c` (4 MB atlas mirrors),
  `tagpu_terr.c` (`s_rgbMirror`, "up to 23 MB held for the process"), `tagpu_posedraw.c`,
  `tagpu_posebake.c`, `tagpu_render3do.c` — for a consumer that no longer exists. Those latches now
  ask **`tagpu_vk_owns_present()`**, which is the real question ("will a Vulkan pass run in this
  process?"). What the lever still legitimately does on that path is ungrey the menu's GPU row,
  whose choice applies to a launch that picks `renderer=vulkan`. It is **not** read for its `color=`
  there — `read_lever` is reached only from `tagpu_vk_frame`.

#### Not covered by 4d-1

- **The GL capture half is still in the tree and now has nothing to pair with.** `tagpu_abshot.c`
  still writes `tagpu_<pass>_gl.ppm` from eight call sites. Deleting it is **4d-2**, and the owner's
  decision (2026-09-18) is that `tagpu_vk_shot.c` and the eight `.ab` levers **stay** — a
  single-lane capture is still how a PPM of the Vulkan frame is taken for the cross-build
  comparison this plan now depends on.
- **`tools/vkcoexist.c`'s route A and route D cases** describe an arrangement the DLL no longer has.
  The harness is untouched and still measures what it always did; it is the interpretation that has
  narrowed.
- **The GL half of a frame-time comparison can only come from an older build now.** Nothing in the
  tree can produce one, so the "no worse than GL" clause is answered by comparing against a figure
  taken before 4d-1 rather than by a measurement this build can repeat.

### 2.56 The GL capture half goes, and gate 4's deletions are done — landing 4d-2

**LANDED 2026-09-18.** 101 lines in, 599 out. `tagpu_abshot.c` and `tagpu_abshot.h` are deleted
(392 lines of the total), and with them every `TAGPU_ABSHOT` declaration, `tagpu_abshot_begin` and
`tagpu_abshot_end` across the eight ported passes.

#### It was already unreachable, and 4d-1 is what made it pointless

The captures sat behind `gl_draws`, which is `!tagpu_vk_owns_present()` — false under
`renderer=vulkan`, where there is no GL context to read a framebuffer from. What made the file
*pointless* rather than merely idle is §2.55: route D gave the two lanes a window each, so one frame
could be photographed from both sides and diffed, and with route D gone the GL half had nothing to
pair with.

#### Every pass already contained the code that survives

That is what made the landing small, and it is worth recording as a shape. All eight carried **two**
branches — the GL one that captured and claimed on `wrote && fresh`, and a second that merely
claimed — because landings 4b-1…4b-3 had already been forced to write the second one for the lane
with no GL half. The deletion is each pass collapsing onto its own existing branch:

```c
if (taking) { s_abDone = 1; s_abFrame = tagpu_vk_ab_arm("<tag>"); }
```

**The spelling differs and it matters when you go looking.** Five of the eight — `terr`, `feat`,
`fx`, `fps`, `scaffold` — had a literal `} else if (taking) {`. The other three had a plain `} else {`
nested inside an already-open `if (taking)` (`mark`, `gui_surf`) or `if (s_abTaking)` (`posedraw`).
And `posedraw`'s flag is **`s_abClaim`**, not `s_abFrame`, guarded by `s_abTaking` — its own
`s_abFrame` turned out to be write-only in this tree *and* on `main`, so it went too.
[BOTH CORRECTIONS FROM THE 4d-2 LANDING REVIEW, against my own over-generalised first draft.]

#### What stays, and it is a decision rather than a default

The **Vulkan half and the eight `.ab` levers stay** — the owner's call, 2026-09-18. A single-lane
capture is still how a PPM of the Vulkan frame is taken, and the cross-**build** comparison that
replaced the two-lane oracle is what this plan leans on from here. `tagpu_vk_ab_arm` is untouched:
it unlinks the target `_vk.ppm` at the instant the claim latches, which is what makes the file on
the disk *this* arming's rather than an earlier run's.

#### Verified by running it

- The full play arm set renders a complete 1024x768 frame on `renderer=vulkan` — **0 magenta pixels
  of 786 432**, with the clear colour left at its magenta default.
- `tagpu_terr.ab` alone writes `tagpu_terr_vk.ppm` at **2048x1536** — `gw*ss` by `gh*ss`, the world
  target's own size — and the lane logs `vk: shot: wrote tagpu_terr_vk.ppm, 2048x1536`.

**A first attempt at that second check looked like a regression and was not**, which is worth
writing down because the next session will repeat it. Arming the lever with the FULL arm set
produced no file at all; the seam's answer was *"1 A/B levers claimed this frame and 5 passes drew
into it - nothing captured"*. That is the "one pass per capture" rule from G19e. The claim fired
correctly; the fixture was wrong.

#### Not covered by 4d-2

- **`s_curDrew`'s ordering is still open**, and it is now the only thing left in gate 4.
  `render_vk.c` publishes `tagpu_cursown_publish(tagpu_gui_cursor_drew_take())` **before**
  `tagpu_vk_frame`, so it asserts "our cursor was drawn" on the strength of the mirror record
  reaching `draw_layer`'s tail while `tagpu_vk_gui_prepare` can still refuse the frame afterwards.
  The fix is an ordering — hold the taken value, have the seam report whether
  `tagpu_vk_gui_record` actually ran, and publish `want && composited` after the frame, so every
  early return yields the truth instead of a stale latch.
- **The two captures are still labelled "GL" and "Vulkan" throughout `tools/vk-ab.py`**, which is
  what a cross-build pair is not. Its gamedir form now refuses by name and its over-draw diagnostic
  reads in cross-build terms, but the column headings and variable names still say GL. Renaming
  them is a change to a tool every session reads, and it can ride a later landing.
- **`tools/uiwalk.py`'s `--vk` walk is gone entirely**, not just refused: `Walk.vk_ab` (119 lines),
  the per-stop `vkdiff`/`vkink` plumbing and the report's Vulkan-against-GL table went with it.
  The flag still refuses at argument-parse time and says what replaced it.

### 2.57 `s_curDrew` publishes after the frame it reports — gate 4's last item

**LANDED 2026-09-18.** Named by the 4b-3 landing review, assigned to 4c by the plan, and closed by
none of 4c-1…4c-3 or 4d-1…4d-2. It is the last thing in gate 4.

#### The gap

`render_vk.c` published `tagpu_cursown_publish(tagpu_gui_cursor_drew_take())` **before**
`tagpu_vk_frame`. That told the engine "our cursor was drawn" on the strength of the mirror
*record* surviving to `draw_layer`'s tail — `tagpu_gui_surf.c:2547` is
`if (s_sharpOn && s_curInLayer && (gl_draws || s_mirRec)) s_curDrew = 1;`, and on this lane
`gl_draws` is 0, so the latch rests on `s_mirRec` alone. `tagpu_vk_gui_prepare`, inside the frame
that had not run yet, can still refuse it. On those frames nothing composited and the engine's own
cursor had already been suppressed — **no cursor at all**, the fail-closed shape `tagpu_cursown`
exists to avoid.

**There are more refusal paths than the first draft of this section listed, and they are not the
ones it named.** It said "`s_behind`, `!s_engHave`, `!s_palHave`, a presented twin that stood down
or resized, either shader-refusal path" and then spoke of "the five". Those last two are
**sharp-layer hand-over** refusals (`tagpu_vk_gui.c:1622-1626`, the cursor wanting an atlas nobody
handed over, and `:1628-1643`, the minimap wanting its pair) — nothing to do with shader
compilation, and a session sent looking for a shader refusal will not find one. Nor is the list
exhaustive: `compose` is cleared at five more sites, `prepare` returns 0 outright at six, and
`s_uiDrew` is also 0 when the seam has no render pass or framebuffer. Treat it as "many, and the
counter says how often rather than which". [CORRECTED BY THIS LANDING'S REVIEW.]

#### It is an ordering, not a move, and the difference is the whole fix

The publish's position is argued in place: *"the only point every path through the driver reaches,
and a flag published inside the UI present would keep its last value across the driver's three
early returns."* Relocating it would break what that argument protects. So:

- the **take** stays where it was, because taking is what *clears* the producer's flag and that has
  to happen once per iteration;
- the seam reports whether the UI layer reached the command buffer — `tagpu_vk_ui_composited()`, a
  latch cleared as the **first statement** of `tagpu_vk_frame` so every early return answers 0, and
  set from **`tagpu_vk_gui_record`'s RETURN VALUE**. The first draft set it beside the call, and
  this landing's own review found the hole: `record` has a refusal `prepare` cannot exclude — a
  composite pipeline that would not build — and it **latches** (`s_layRp` is cleared, so every
  later frame re-enters and fails identically). Setting the flag on "we called it" would have
  reported a composite for the rest of the session on exactly the failure this landing exists to
  remove, with `held` reading 0 throughout: the instrument blind to the one case it was added for.
  `record` returns `int` now, 1 only after its `vkCmdDraw`;
- the publish happens **after** the frame, on the loop path every iteration reaches, with
  `cur_drew && composited`.

#### A number, not a claim — and why the counter is permanent

The frames this changes are invisible from outside: the refusal window could not be forced by hand
and the GUI heartbeat is far too coarse to sample it. A first attempt at verifying by photographing
the shell proved nothing — `cursown=4/4,**1**` showed the engine's blit *was* suppressed, so the
cursor in the picture was ours and the GUI pass had already recovered. So the case is **counted**:
`tagpu_cursown_note_held`, surfaced as `held=` in the heartbeat's `cursown=` field. It is a
permanent instrument in the shape this tree uses everywhere, not scaffolding.

| measured, `renderer=vulkan`, full play arm set | |
|---|---|
| at the shell | **`held=1`** |
| after walking into a live game | **`held=3`** |
| the in-game frame otherwise | 1024x768, **0 magenta of 786 432** |

Three frames a session on which the engine's cursor was being suppressed with nothing of ours on
screen.

`render_ogl.c`'s own publish is untouched and needs nothing: there `draw_layer` sets `s_curDrew`
after the composite has really run, and since 4d-1 that backend never drives this lane.

#### Not covered

- **Which refusal path those three frames took is not recorded** — the counter says how often, not
  why, and there are more than a dozen candidates (above). The two after the shell straddle the
  level load, so a twin reseed is the likely cause, but that is inference rather than measurement.
- **A frame whose submit or present fails still publishes 1**, because `s_uiDrew` is set once the
  composite reaches the command buffer and the failure is after that. It is self-correcting — the
  next frame clears the flag at the top of `tagpu_vk_frame` and returns early, and the loop's exit
  publishes 0 — and the presentation engine keeps showing the last good frame, which carried our
  cursor. It is the same shape as the pipeline-build hole this landing's review found, and the only
  reason it is benign is that it does not latch. [RAISED BY THAT REVIEW.]
- **`s_curDrew` is still a latch the producer sets from a record**, not from a composite. This
  landing makes the *consumer* honest about the frame; it does not change what the producer means.

### 2.58 A solid rectangle is a colour and a box — landing 8a of the Vulkan-only plan

**LANDED 2026-09-18.** The first part of landing 8, and the only one of its five op kinds whose
shape is unambiguous.

#### The gap: the bytes were both larger than the op and later than it

`tagpu_gui_hook.c`'s observer records `OP_BAR` from `DrawBar 0x4BF6F0` — a clipped solid fill,
edges inclusive, 47 callers, the unit health bars among them. `publish()` had no case for it, so
it fell through `as_pixels:` to `PK_PIXELS` + `pub_surface_bytes`: **a `memcpy` of the op's box
out of the live engine surface, taken at the FLIP.** Two things are wrong with that and only one
of them is size.

- **Later than the op.** The op happened inside `DrawBar`; the copy happens when the engine flips.
  Anything drawn over that box in between is what the bytes hold, so the twin receives a picture
  of the *final* surface cropped to an *earlier* op's rectangle.
- **Larger than the op.** A 35×5 health bar is 175 bytes of arena for a fill that is one index.

This is §20 of [gui-renderer](../gui-renderer.html)'s chain — the cyan squares — with the same first
three links and a different fourth. There the box was a line's bounding box and the bytes it
scooped up were the terrain key; here the box is right and the bytes are still the wrong ones.

#### One byte is the whole colour, and that is the engine's width, not a choice

Landing 8a is a packet with a colour field in it, so the field's width had to be **established
rather than assumed**, and that was done first and landed separately (`f8c1b6b`,
[exe-reverse-engineering](../exe-reverse-engineering.html)). `DrawBar` clips with `0x4BF620` and
writes with `0x4CCDEA`, which takes **the low byte of its colour and nothing wider**: the 4-aligned
path (`0x4CCE2D`) builds a DWORD of four copies of `BYTE PTR [ebp+0x10]` for `rep stos DWORD`, and
the unaligned one (`0x4CCE73`) loads the argument as a DWORD and stores `rep stos BYTE al`. So
`TAGPU_PUBOP::fg` is one `unsigned char` and that is exactly what the engine keeps for `OP_BAR`.

**AND THAT IS A FACT ABOUT `DrawBar` ALONE — the version of this section that shipped with the
code claimed it for four functions and was wrong about three of them.** [CORRECTED 2026-09-18 BY
THIS LANDING'S REVIEW.] It said `DrawTranspRectangle 0x4BF8C0`, the framed box `0x4BF4D0` and the
focus rectangle `0x4BF7B0` all wrote through `0x4CCDEA` too. Counting the calls in each body:
`0x4BF8C0` makes ten to `0x4CC7AB` and none to `0x4CCDEA`; `0x4BF7B0` makes eight to `0x4BEC70`
and none; `0x4BF4D0` makes none and does not fill at all.

**`0x4BF4D0` is a SHADE, and its third argument IS a mode.** It clips once through `0x4BF620` and
then runs a read-modify-write over the box: `movsx ebx,BYTE PTR [ecx]` / `mov bl,[row + ebx]` /
`mov [ecx-1],bl`, where `row` is one of **32 rows of 256 bytes** — `globals+0xC4` for a negative
level (row `level + 0x20`, clamped at `-0x20`) and `globals+0xC8` for a positive one (row `level`,
clamped at `+0x1F`). A NULL table returns 0 and draws nothing.

**So `0x4AA912`'s `0x4BF4D0(panel+0xBC, rect, -0x18)` is darken level 24, not palette index 232**,
and the survey that preceded this landing — which flagged that argument as possibly a shade mode —
**was right and was overruled on a false reading**. It was overruled by disassembling `0x4CCDEA`,
a function `0x4BF4D0` does not call. The lesson is narrower than "check the disassembly", because
the disassembly *was* checked: **the reading of one function was generalised to three others by
their resemblance, and the resemblance was the whole of the evidence.**

None of this reaches the shipped path — `publish()` reads `col` for `OP_BAR` only — but the
observer now records the raw third argument for all four leaves, so the comments at `OP::col` and
`rect_box` say per kind what it means.

#### Five parts, because the consumer has two layers

| part | file | what it is |
|---|---|---|
| capture | `tagpu_gui_leaves.h` `rect_box` | `s_lastOp->col = (unsigned char)ARG(e, 3)`, taken while the engine is inside the call |
| packet | `tagpu_gui_int.h`, `tagpu_gui_hook.c` | `PK_BAR` — the box in `l,t,r,b`, the index in `fg`, **nothing in the arena** |
| GL twin | `tagpu_gui_surf.c` `twin_fill` | scissored `glClear` to (index, coverage 1) in the RG pair |
| mirror op | `tagpu_gui_surf.c` `drain` | `TAGPU_GUIOP_BAR`, recorded where the op is APPLIED |
| Vulkan draw | `tagpu_vk_gui.c` `prepare` | `vkCmdClearAttachments` over the rect |

**The capture uses the decoration `gaf_box` and `before_copy` already make**, and its safety is the
one those rest on: every early return in `op_add` leaves `s_lastOp` NULL, so a clipped-away or
surface-less op cannot write its colour into the *previous* op. That is an ordering the producer
already guarantees, not a new one.

**`twin_fill` is `twin_clear`'s shape with two differences**, both stated in place: the index goes
in RED (the twin is RG — index and coverage) and GREEN is **1**, because this op *covers* what it
fills where a clear *uncovers* it. It is pure GL and returns immediately under
`tagpu_vk_owns_present()`, for `twin_clear`'s reason — it feeds nothing the Vulkan twin is told;
the mirror op beside the call is what does that.

**The Vulkan half shares `TAGPU_GUIOP_CLEAR`'s path, and that is the cheap part.** Both are
`vkCmdClearAttachments` over a rect, so neither takes a draw slot: **no quad, no descriptor set,
and not counted against `DRAW_MAX`/`QUAD_MAX`.** The index attachment is `VK_FORMAT_R8G8_UNORM`,
so `clearValue.color.float32[0]` and `[1]` are index and coverage — `fg/255` and `1.0`. The
**colour attachment still goes to zero on both kinds**, for CLEAR's own stated reason: a solid
engine fill has no restored art under it, and leaving old colour standing would show it through a
box the engine has just painted over.

#### Measured, and then the honest part

| measured, `renderer=vulkan`, full play arm set, 1024x768 | |
|---|---|
| ops observed in one live game (census) | **`bar 1334`** |
| replayed as geometry (`bars=` in the GUI heartbeat) | **158** |
| the frame | **0 magenta of 786 432** — the health bars visibly under each unit |

**And this is 0.02 % of the traffic.** The same census reads **`rect 5 396 343`** and
**`line 3 614 453`** against that `bar 1 334`. `OP_BAR` was taken first because it is the only one
of landing 8's five kinds with an unambiguous shape — one engine function, one solid fill — where:

- **`OP_RECT` conflates two engine functions**: `DrawTranspRectangle 0x4BF8C0` is *hollow*, four
  edges through `0x4CC7AB` rather than one fill through `0x4CCDEA`, and the focus rectangle
  `0x4BF7B0` is a third shape again.
- **`OP_FRAME` (`0x4BF4D0`) is not a fill at all**, as above: it tints what is already in the box
  through a 32-row LUT. It cannot become a solid rectangle with a colour in it, and the packet it
  needs is a *level* plus whatever makes the destination readable — which on the Vulkan lane is a
  twin it would have to sample and write back. That is a different landing from 8b and 8c, and
  saying so is the most useful thing this landing produced for the ones after it.
- **`OP_LINE` needs the direction bit**, because a diagonal's bounding box is not the line — which
  is precisely the fault §20 of [gui-renderer](../gui-renderer.html) traced.
- **`OP_SCALE` is a scaled blit** and probably belongs with `PK_SPRITE` rather than with these.

So 8a proves the mechanism end to end at a volume that does not matter. **The volume is in 8b
(rect) and 8c (line)**, and this section exists partly so the next session does not read "landing
8 has started" as "landing 8 is nearly done".

#### Not covered

- **Four of the five op kinds still publish surface bytes.** `OP_LINE`, `OP_RECT`, `OP_FRAME` and
  `OP_SCALE` are unchanged, and with them the whole of the ~9 M ops a game that made landing 8
  worth filing. `PK_PIXELS` is not closed; one kind left it.
- **`twin_fill`'s GL body did not execute in EITHER measured configuration, and that is measured
  rather than argued.** Under `renderer=vulkan` it returns at its first statement by design. Under
  `renderer=openglcore`, a full game logged **`bars=0` against `bar 2348` observed** — every
  `PK_BAR` was dropped by `drain`'s `twin_find(o->surf)` before reaching it. Side by side, one live
  game each, both lanes at `twins=2`:

  | | Vulkan | GL |
  |---|---|---|
  | `bar` observed | 1 218 | 2 348 |
  | `bars=` replayed | **146** | **0** |
  | `clears=` | **0** | **145 762** |
  | `seeds=` | 20 | 15 |

  **The inversion is pre-existing and not this landing's**: `PK_PIXELS` carries the *identical*
  `twin_find` guard, so these ops were dropped on the GL lane before 8a exactly as they are now —
  the landing did not change their fate, it added the counter that makes the fate visible. What it
  means is that the two lanes twin *different* surfaces, and neither the bars' surface on GL nor
  the clears' surface on Vulkan is among them. **Not traced.** It is the next thing to look at
  before 8b, because 8b's ops take the same path and will meet the same guard.
- **So the review's `glClearColor` finding was a latent fault, not a live one**, on the evidence
  above — the fix still belongs in, because reachability is not the argument for leaving global GL
  state modified, and `bars=0` is a fact about one configuration rather than a proof about all of
  them.
- **`bars=158` against `bar 1334` is not a defect, and the reason is the publisher's dedup.**
  Both are cumulative since process start — `s_kindTotal[OP_BAR]` in the observer, `s_bars` in the
  drain, neither ever reset — but they count different things either side of `publish()`. The
  observer counts every `DrawBar` the engine makes; `publish()` **keeps only the LAST of any run of
  identical ops** in a batch (`tagpu_gui_hook.c:1089`, measured at ~41 redraws a flip in the
  shell), and a stationary unit at unchanged health redraws the same bar every flip. `bars=`
  further counts only ops whose surface still has a twin. The two lines also print on different
  cadences — `gui:` every 300 frames, `GUI kinds:` every 600 — so they are not read at the same
  instant either. Nothing in this landing needs them to agree; the ratio is stated so the next
  reader does not take it for a loss.
- **`gui.on=strict` renders the units as the lane's clear colour on this build.** Observed while
  measuring 8a, **pre-existing and untouched by it** — plain `gui.on` on the same binary is 0
  magenta. It is recorded here because it was first misread as a regression of this landing: two
  variables had been changed at once. Unexplained, and it belongs to whichever landing next has a
  reason to arm `strict` as its oracle — which landing 8's plan entry does.

### 2.59 A hollow rectangle is four edges — and the census says landing 8 is not what it looked like — landing 8b

**LANDED 2026-09-18.** `OP_RECT` becomes `PK_RECT`. The larger result is what splitting it
revealed about where the traffic actually is.

#### `OP_RECT` was two engine functions and they are not the same operation

The observer mapped **both** `0x4BF8C0` and `0x4BF7B0` to `OP_RECT`. Disassembled:

| | `0x4BF8C0` `DrawTranspRectangle` | `0x4BF7B0` the focus rectangle |
|---|---|---|
| edges | 4, one box | **4, one box** — the eight `call 0x4BEC70` in its body are two *mutually exclusive arms* on `ctx == NULL` [corrected by this landing's review] |
| writer | `0x4CC7AB`, **store-only** Bresenham | `0x4BEC70` → `0x4CC8DF` |
| reads the destination? | **no** | **yes** |
| third argument | a **palette index** (`[ebp+0x1C]`, `stos BYTE al` — the low byte) | a **shade level** into `globals+0xC8` |
| portable as geometry? | yes | **no** |

`0x4CC8DF`'s inner loop is `shl eax,8` / `mov al,[edi]` / `mov al,[eax+esi]` / `mov [edi],al` —
it builds `(row << 8) | destination` and looks the result up in the **same lighten table
`0x4BF4D0` selects from** (§2.58). So the focus rectangle tints what is under it and has no
colour of its own.

**A negative result that will bite whoever ports the tints:** the LUT index is **unsigned** in
`0x4CC8DF` and **signed** in `0x4BF4D0` (`movsx`). One table, two addressing conventions.

**And a second one, found by landing 8d's review rather than by the port:** because `0x4CC8DF`
*reads* its destination, a tint is the first op in this stream that is **not idempotent**, and
two places in the fork quietly assumed every op was. `dedup` collapses ops that compare equal —
correct for every kind that overwrites its box, wrong for a tint, whose two identical copies are
`LUT[LUT[x]]` and not a slower route to `LUT[x]`; a one-pixel-tall focus rect produces exactly
that pair **inside a single call**, since at `t == b` the top and bottom edges carry the same
box. And the drain drops `PK_PIXELS` by design, so a tint replayed over a box that failed to
repaint folds each frame's error into the next frame's input. Neither is visible as a wrong
pixel on the frame it happens; both compound. Whoever ports the *other* read-modify-write
(`0x4BF4D0`, one table along) inherits both questions unchanged.

So `before_focus` is its own op kind now — `OP_FOCUS` — and `OP_RECT` means exactly `0x4BF8C0`.
That split is the whole of what made 8b portable, and it is also what produced the next section.

**`OP_SCALE` is the one kind here that is semantic for some of its ops and not others**
[landing 8e, 2026-09-21]. `GAF_DrawTransformed 0x4C7580` maps a frame onto a quad, and only the
axis-aligned cases can be replayed as a sprite; the rest still publish nothing. What 8e changed is
which axis-aligned cases count — the uv quad is a **window**, so a sub-rectangle of a frame is a
source like any other, and `SKIRMISH.GUI`'s player swatches (a 30×30 inset of a 32×32 frame at
19×19) were the shell's entire remaining residual for want of that one reading. Two consequences
worth carrying: the consumer's atlas key `(frame, pix, w, h)` is **not** an identity once windows
exist, and the census cannot sum this kind as though it were always semantic — it counts the
captured and uncaptured halves apart.

#### What the census actually says, now that the two are counted apart

One live game, `renderer=vulkan`, full play arm set, 1024x768:

| op kind | engine function | count | status |
|---|---|---|---|
| **`focus`** | `0x4BF7B0` — **a tint** | **1 223 310** | **`PK_TINT` since landing 8d, 2026-09-21** (was: "`PK_PIXELS`, and cannot leave it as a colour and a box" — right about the colour, wrong that nothing else could carry it) |
| `line` | `0x4BE950` | 842 790 | `PK_PIXELS` — landing 8c |
| `copy` | `0x4C6B70` | 414 729 | already `PK_COPY` |
| `rect` | `0x4BF8C0` | **3 476** | **`PK_RECT` — this landing** |
| `bar` | `0x4BF6F0` | 1 184 | `PK_BAR` — landing 8a |
| `scale` | | 6 819 | `PK_PIXELS` — **the only one left**, and the last `raw` in the shell |
| `frame` | `0x4BF4D0` — a tint | 0 this run | `PK_PIXELS`; `PK_TINT`'s shape one table along — a FILLED box through `globals+0xC4` when its level is negative — so it needs the branch and no new mechanism |

**The `focus` row's verdict was overturned three days later and the way it was wrong is the useful
part.** "Cannot leave it as a colour and a box" is true; "and therefore must publish the
destination's bytes" does not follow, and that second step was never stated — it was assumed.
What crosses instead is the **operation**: a box, a row, and the 32 × 256 lighten table once
(`PK_TINT` / `PK_SHADE`). The consumer re-derives the remap on its own twin, so nothing the
engine composed crosses and the clean cut is untouched. The count above also changes meaning
after 8d: the observer now records the four EDGES of each focus rectangle as four ops, so `focus`
counts four times as many ops covering a quarter as much area each, and the area figure is the
pixels the engine writes rather than the box it writes inside. Full write-up in
[the GUI renderer note](../gui-renderer.html).

**The `rect 5 396 343` this plan has quoted since the survey was the two functions added
together, and it is ~99.7 % the focus rectangle.** 8b was filed as "the volume"; it is 3 476 ops.
The volume is a **tint**, and a tint is the one shape in landing 8 that cannot become "a
description of a draw" without the consumer being able to read what it draws over.

**This is the second time on this gate that counting something apart moved the plan** — §2.58 did
it to `OP_FRAME`. Both times the cause was the same: an op kind named for what it looked like
rather than for the engine function behind it.

#### The port

`PK_RECT` carries the outer box and one palette index, nothing in the arena. Both consumers draw
**four edges and never the interior** — that is not an optimisation, it is the difference: the
interior is whatever was already in the twin and the op never wrote it, so a fill would be wrong.

* GL: `twin_outline`, four scissored clears, the clear colour restored afterwards (§2.58's fix,
  applied here from the start rather than found by a review).
* Vulkan: **one** `vkCmdClearAttachments` with `rectCount = 4`. It shares `TAGPU_GUIOP_CLEAR`'s
  path, so it takes no draw slot, no quad and no descriptor set. Each of the four rects is clamped
  to the twin independently and the ones that clamp away are dropped, with the survivors compacted
  so `rectCount` stays meaningful — a bound, because a clear rect outside the render area is
  undefined behaviour rather than a no-op.

Corners are written twice, by the same constant, which is what the engine's own four edges do.

#### Measured

| `renderer=vulkan`, full play arm set, 1024x768 | |
|---|---|
| `rect` observed | **3 476** |
| `rects=` replayed | **3 474** (a 2-op sampling gap — the two lines print on different cadences) |
| the frame | **0 magenta of 786 432**, **2 477 distinct colours** |
| by eye | terrain, trees, the HUD panel, the minimap, METAL/ENERGY bars, the commander's health bar |

The colour count is asserted deliberately: §2.58's trap is that "0 magenta" passes on a black
window too.

#### A clipped hollow rectangle keeps the old path, and that is the landing's one real defect

**Found by this landing's review, and it is specific to hollow figures.** `rect_box` clamps the
**whole box** to the context's clip rect (`clip_ctx`). The engine does not: `0x4BF8C0` clips each
of its four edges **separately** through `0x4BEA20`, which draws *nothing* for an edge wholly
outside. So a rect crossing its clip rect is an **open** figure on screen, and replaying the
clamped box would **close** it — with up to two edges painted along the clip boundary the engine
never drew.

**It was not a defect before 8b**: as `PK_PIXELS` the box's real bytes were copied, so whatever the
engine did — including nothing — was what got published. Describing a shape is what introduced the
chance to describe the wrong one. **`PK_BAR` is genuinely unaffected**, and the asymmetry is the
proof the analysis is right: a clipped solid fill *is* the same fill restricted to the clip rect,
while a clipped outline is a different outline.

**Fixed by construction rather than by a coordinate correction:** `OP::clipped` records whether
`clip_ctx` moved an edge, and `publish()` describes the rect as geometry only when it did not
(`op->kind == OP_RECT && !op->clipped`). A clipped rect keeps `PK_PIXELS`, whose bytes come from
the surface and are exact whatever the engine did. That is a fallback the op already had, chosen on
data, not a guess at what the engine would have drawn.

The reviewer could not prove a clip actually occurs at any of the twelve call sites (`0x466B5E`
the minimap view box, `0x467F6C` the HUD, `0x468332`, `0x468457`, `0x46854A`, `0x4686CA`,
`0x4688F2`, `0x469EC5`, `0x469F1E`, `0x46B94D`, `0x46BA67`, `0x4ACA03`) and neither did this
landing — **so the fix is untested against a real clip**, and the honest statement is that it
removes the possibility rather than that it repairs an observed picture.

#### Not covered

- **`OP_FOCUS` still publishes surface bytes**, and it is 1.2 M ops a game — after 8a and 8b the
  single largest consumer of `PK_PIXELS` among the leaves landing 8 was filed to close. Nothing
  here changes that; the landing renames it accurately and measures it.
- **The tints need a mechanism landing 8 does not have.** `OP_FOCUS` and `OP_FRAME` both read the
  destination. On the Vulkan lane that means sampling the twin the pass is writing — a subpass
  input or a second pass, not a clear. **Whether they port at all is open**, and leaving them as
  `PK_PIXELS` remains a defensible answer for exactly the reason they are hard: publishing the
  destination's bytes is what a destination-dependent op actually means.
- **`twin_outline` is unexercised for the same reason `twin_fill` is** (§2.58): on the GL lane
  these ops find no twin. Not re-measured per lane here.
- **No fixture produced a clipped `OP_RECT`**, so the `clipped` fallback above is exercised by
  construction and not by measurement. A fixture that forces one — the minimap view box at a
  zoom-out wide enough to exceed the minimap gadget is the reviewer's candidate — would be worth
  more than another clean run.
- **The 1 px box case is reasoned, not measured.** A box with `l == r` gives four degenerate edges
  that collapse onto the same column; the code clamps extents rather than skipping edges so the
  result is the single column the engine draws, but no fixture produced one.

### 2.60 An axis-aligned line is a bar, and a diagonal is not a box — landing 8c

**LANDED 2026-09-18.** The last of landing 8's portable kinds, and the one the plan warned about:
*"a diagonal's bounding box is not the line"*.

#### The split is the design and it is also the measurement

`before_line` recorded `OP_LINE` as the axis-aligned **bounding box** of the engine's
`DrawLine 0x4BE950`. For a horizontal or vertical line that box **is** the line, one pixel thick.
For a diagonal it is the square the line crosses — and replaying *that* is precisely
[gui-renderer](../gui-renderer.html) §20's cyan squares, where four such boxes tiled a rotated
selection square solid and published the terrain key underneath.

So the observer decides from the **endpoints**, before the box exists, and keeps two op kinds:

| kind | when | published as |
|---|---|---|
| `OP_LINE` | `x0 == x1 \|\| y0 == y1` | **`PK_BAR`** |
| `OP_DIAG` | otherwise | `PK_PIXELS`, unchanged |

**Two kinds rather than a flag, because the census then counts them apart** — the same move that
made 8b honest about `focus`, and the reason the ratio below is read rather than guessed.

**An axis-aligned line is published as `PK_BAR`, not as a kind of its own.** It is a solid fill of
a one-pixel box: same packet, same `twin_fill`, same `vkCmdClearAttachments`. That adds **zero new
enumeration sites** to either consumer — and a kind missing from one enumeration is this
codebase's characteristic silent bug, the thing 8b's review had to go looking for. Two draws that
are the same draw should not be two kinds.

The `OP::clipped` fallback from 8b applies unchanged: a line whose box `clip_ctx` moved keeps
`PK_PIXELS`.

#### Measured, and the diagonal only appears when you force it

| `renderer=vulkan`, full play arm set, 1024x768 | `line` | `diag` |
|---|---|---|
| shell, then a live game | 800 315 | **0** |
| a unit selected | 836 429 | **0** |
| `mark.on=noselbox`, unit selected | 1 070 097 | **0** |
| `mark.on=noselbox`, unit selected **after a diagonal move order** | 1 548 252 | **111 603** |

**`diag` was 0 in every measured session — but that is a RESULT, NOT A PROPERTY, and the first
version of this section stated it as one.** [CORRECTED BY THIS LANDING'S REVIEW.] It said "under
the shipped arm set there are no diagonal UI lines at all", on the reasoning that `markown.on`
suppresses the engine's own selection box. **The suppression is conditional and can lapse.**
`tagpu_markown.c:416-418` suppresses **per unit**, and only while
`tagpu_native_selbox_complete()` — which `tagpu_native.c:4026` computes as
`selDrawn + selNone == nsel && nu < MAXU`, i.e. **0 whenever the native pass came up short** on
the gather cap, on `MAXU`, or on an unresolvable model AABB. `tagpu_native.c:3795-3803` records a
**measured** instance of exactly that: one dying selected unit dropped `s_selComplete`, *"markown
handed all ~460 rects back, and the engine drew every one of them"* [2026-09-09]. So diagonals are
reachable in the shipped configuration, and the honest claim is **0 in the sessions measured; the
engine's box returns whenever `s_selComplete` drops**.

This changes nothing about the code — `OP_DIAG` keeps `PK_PIXELS` and is safe either way — but it
was the stated reason not to hurry porting `OP_DIAG`, and that reason is weaker than it read.

Forcing a diagonal deliberately takes three things together, and any two give zero:
`mark.on=noselbox` so the engine draws its own box, a unit selected, and **an orientation off the
axis** — which is why the move order is in the recipe. **Note "orientation", not "heading":**
`tagpu_native.c:3815` records that the engine hands `0x4B6CC0` all **three** of the unit's angles
(bank, heading, pitch at `u+0x64`), so on a slope a heading that *is* a multiple of 90 still
produces a rotated square. [Also corrected by the review.]

So 8c closes essentially all of `line` in the shipped configuration, and the ~7 % that is diagonal
under the forced one is kept safe rather than ported.

The frame with diagonals present: **0 magenta of 786 432, 2 003 distinct colours**, 60.0 fps,
`bars=69 316` — and **no cyan squares**, which is the specific regression this split exists to
prevent.

#### Not covered

- **Diagonals still publish surface bytes.** Porting them needs a line rasteriser in the twin —
  a real draw, not a clear — and it has to match `0x4CC7AB`'s Bresenham exactly or the A/B moves.
  Nothing here starts that.
- **`diag` was measured on one map with one unit.** 111 603 is what a forced fixture produced, not
  a figure about play. And the 0 above is what the measured sessions produced, not a guarantee —
  see the correction above.
- **The `s_selComplete` lapse was never measured for its EFFECT on `diag`.** That it hands the
  boxes back is measured (2026-09-09); that the handed-back boxes are diagonal often enough to
  matter is inference. Forcing the lapse — a dying selected unit, or a selection past the gather
  cap — and reading `diag` off `GUI kinds:` is the measurement nobody has taken.
- **The 1-px-thick claim is about the BOX, not about `0x4CC7AB`'s raster.** An axis-aligned line's
  bounding box is one pixel thick by construction; that the engine's Bresenham fills exactly that
  box is inherited from the existing engine-map entry (both endpoints counted, `inc ecx` on both
  axis-aligned special cases), not re-derived here.

### 2.61 The engine can be asked to redraw, so the shell's art arrives as draws — landing 9

**LANDED 2026-09-18.** The first thing in this tree that **calls** an engine draw function rather
than watching one.

#### The gap

A surface whose contents we did not watch arrive can only be published as **`PK_SEED`** — an
empty twin, since its bytes do not cross (§2.104) — because nothing here knows how they got there
(`tagpu_gui_hook.c`'s `pub_seed`). That is not an edge case: `publish()` seeds *any* surface on
first touch, and a **reseed** clears `seeded` on every surface at once. Four things ask for one —
the consumer stalling over, a lost sprite, an arena overflow, and a level boundary — so seeds are
republished repeatedly through a session.

Seeded bytes are flat. They carry no frame identity, so the GUI atlas never holds them, and
neither the restorer nor the sharp layer has anything to work on. That is G15e's *"seeded art
stays indexed until repainted"*, and it is what this closes.

#### The mechanism

`GUI_StageUpdateDraw 0x4A81E0(gi, 0x40)` is the engine's own redraw of the **top** screen. Every
draw it makes runs through the leaves we already have, so the screen's art arrives as ops.

| what makes it safe | why, and it is not a timing argument |
|---|---|
| **a bound on the flag** | `0x4A82F0` computes `flags & 1` and `0x4A82F7` jumps past *both* allocations (`0x4A907C`, `0x4A90B5`). The frees are **six** sites, not the two this row first named [CORRECTED by the review]: `0x4C6AC0` at `0x4A9537`/`0x4A9549` **and the raw `0x4D85A0` at `0x4A9575`/`0x4A95A7`**, which free each gadget record's own pointers. All four sit inside one block gated by `0x4A950A test bl,0x2`, with `ebx` reloaded from the flags argument at `0x4A9434`/`0x4A94C0`/`0x4A94C9` — so the conclusion holds on the **gate**, not on a count. Verified over the whole body: 32 direct call targets, no indirect calls, the two indirect jumps are in-body switch tables. **Not claimed:** that nothing it *calls* allocates |
| **a bound on the destination** | the redraw's first act writes `panel+0xBC`, and `0x4C6B70` with a NULL destination resolves to the **primary surface** — a redraw of an unbuilt screen would paint the wallpaper onto the frame. `repaint_service` refuses unless `TheActive_GUIMEM`, its `ControlsAry` and `panel+0xBC` are all present |
| **an ordering** | it runs at the flip's **return**, on the game thread, with `s_inFlip` already cleared. The leaves drop every op while `s_inFlip` is set (the cursor is drawn inside the flip), so a repaint issued anywhere inside it would draw and publish nothing. `s_repainting` makes the call non-reentrant by construction |

**The trigger is `g_guiq.resets`, not the level generation.** The plan said "after arm and after a
level-change reset"; the level case is a *subset*. A seed happens when `publish` finds
`!s->seeded`, and the only thing that clears that for every surface at once is a reseed —
`publish`'s own level check is one of four things that asks for one. `g_guiq.resets` is bumped by
`publish` on the same thread, so watching it needs no new cross-thread agreement and covers the
level case for free. It also *works*: the packet's level generation advances in
`tagpu_packet_pub_level_end`, i.e. when a level is **torn down**, so shadowing it fired once per
session — at the arm — and never on entering a game. It did not need to: entering a game *builds*
the in-game screen, and a build already draws every gadget through the leaves.

#### What it is NOT, and the plan said otherwise

*"Force one repaint and every pixel arrives as an op"* is **false**, and the disassembly says so
before any measurement does. With `0x40` the path always reaches `0x4A90F4`, which repaints the
**whole panel surface from a bitmap** before a single gadget is drawn —
`0x4C6B70(panel+0xBC, GUIMEM+0x24, 0, 0)` when that field is set, else the picture handler
`0x4B0230(gi, 0, panel+0xC4)`. The gadget **chrome** comes back as draws; the **wallpaper** comes
back as a copy, and that copy is only a win while its source is a surface we hold a twin for.

#### Measured

`renderer=vulkan`, 1024×768 in game / 640×480 shell, the shipped arm set, three boots per arm:

| | `norepaint` | shipped |
|---|---|---|
| GUI atlas frames held at the mirror-arm event | **19** | **28** |
| repaints per boot to the shell | 0 | 3–4 |
| shell frame | 148 colours, 0 magenta | 148 colours, 0 magenta |
| in-game frame | — | 1024×768, 709 colours, 0 magenta |
| ops dropped / arena overflows / consumer stalls | **0 / 0 / 0** | **0 / 0 / 0** (three boots each) |
| `builds=` per census window, 3 boots each | 28.1, 21.8, 62.1 | 1.0, 72.0, 69.1 — **overlapping; settles nothing** |
| `buildFlags`, every window of every boot | `0xC0` | `0xC0` |

The ops one redraw produces, by kind:

| top screen | ops | by kind |
|---|---|---|
| `MAINMENU.GUI` (at arm) | **115** | `gaf 105  line 4  focus 6` |
| the shell one reset later | **40** | `gaf 29  line 4  copy 1  focus 6` |
| `ARMMAIN2.GUI` (in game) | **1** | `gaf 1` |

**+9 atlas frames** is the whole of the benefit and it is the shell's. The in-game figure is the
more useful fact: `ARMMAIN2.GUI` is a **three-label screen** (`KILLS`, `LOSSES`, `TOTALUNITS`), not
the HUD — the in-game panel art is drawn by the in-game draw path, not by this gadget tree, so a
repaint there has nothing to re-issue. **A forced repaint is a shell mechanism.**

#### Not covered

* **`PK_SEED` is not closed and this landing does not close it.** A surface is still seeded on
  first touch after every reseed; the repaint replays *over* that seed rather than instead of it.
  What changes is that the art also exists as sprite ops, so the atlas holds it.
* **The wallpaper.** `GUIMEM+0x24`'s surface is not itself established as one we twin; when it is
  not, that copy falls back to bytes like any other.
* **A first run showed 8 "lost sprite" events without the repaint and none with it, and it did
  not reproduce** — a second control boot had 0. Not claimed.
* **Whether the repaint measurably improves what the restorer or the sharp layer produces** was
  not measured; only that the frames are in the atlas for them to reach.
* **A repaint is DESTRUCTIVE to the panel surface by design, and nothing here bounds what that
  costs on a screen we have not measured.** It repaints the whole surface from `GUIMEM+0x24`
  (or the picture handler) before drawing a gadget, then redraws only the gadget tree — so
  anything that reached that surface by another path is erased. This section's own sentence
  *"the in-game panel art is drawn by the in-game draw path, not by this gadget tree"* is exactly
  the condition under which that would bite, on an in-game screen that *does* have `GUIMEM+0x24`
  set. Not observed — `ARMMAIN2.GUI` measured 1 op and no `copy`, so the field was NULL there —
  but it is the residual. [Named by the landing review.]
* **Whether a forced repaint AMPLIFIES the engine's own redraw rate.** A `0x40` redraw sets
  `gi+0xCCA` and the pump answers it with another redraw, so the question is real. `builds=` was
  the instrument and it does not resolve it: three boots per arm gave overlapping means with no
  direction. What is known is that the engine already redraws continuously on its own
  (`buildFlags` 0xC0 everywhere, three engine call sites passing `0x40`), that our repaint fires
  2–4 times a boot on a reseed edge rather than per frame, and that no runaway was observed.
  Settling it needs an instrument that separates the pump's redraws from the rest — `builds=`
  counts every entry and cannot.
* **The transitive allocation question.** The function itself allocates and frees nothing under
  `0x40`; whether any of its 32 callees does is open. `0x4BBC40` and `0x4BBE50` both reach
  `0x4D85A0`, and whether either is on the `0x40` path was not established.

### 2.62 TA's frame was uploaded twice a frame, and one of them was ours — landing 10

**LANDED 2026-09-18.** The plan filed this as *"the engine-frame fallback layer goes from the
composite"*. That is not what the code turned out to need, and the row says so.

#### What the survey actually found

`tagpu_vk_surf.c` holds TA's 8-bit surface as an **R8 image** and draws it opaque as the frame's
bottom layer — `:399` *"This is the frame's BOTTOM layer"* and `:407` *"OPAQUE, AND IT MUST BE"*
say so in place — and in the Vulkan lane nothing else draws it, so it is load-bearing and does
**not** go. `tagpu_vk_gui.c` uploaded the **same bytes again** into its own
`s_engImg`, purely to bind as the layer shader's `uSurf`:

| | source | format | destination |
|---|---|---|---|
| the bottom layer | `tagpu_surf_frame()` → `tagpu_vk_surf.c:636` | `R8_UNORM` | that pass's per-slot image |
| `uSurf` | `tagpu_surf_frame()` → `tagpu_gui_surf.c:3100` → `tagpu_vk_gui.c:1990` | `R8_UNORM` | `s_engImg`, bound at binding 42 |

Same source, same format, same dimensions. At 1024×768 that is **786 432 bytes memcpy'd
host→staging and copied staging→image a second time, on EVERY frame** — not only on frames the
surface changed. Only `tagpu_vk_surf.c` skipped on the serial (`:626-628`); the UI pass's gate was
`if (h.eng) { engUp = 1; … }` under a comment saying *"the engine's frame moves every frame by
definition, so it needs no serial"*. So on a paused or idle game, where surf's skip fires and this
one did not, the saving is the full 786 432 bytes against zero. [The "on every frame the surface
changes" this paragraph used to say under-stated it — found by the landing review.]

**The reason the code gave for it had already died.** `tagpu_vk_gui.c:1656` read *"without it the
GL lane's `uSurf` reads an image ours would not have"* — and the GL lane went in 4d-1/4d-2.

#### What `uSurf` is still for, and why it stays

Not a layer. `LAY_FS` ends in `discard`, so where the twin has no coverage the shader paints
nothing and whatever is underneath shows through. `uSurf` is **read as a reference** by two
consumers: the **stale-mirror guard** (`:630-635`, under `uGuard`), which discards where the twin
says index 0 and the engine says otherwise — this is what stops the layer painting stale black
over the intro Smacker — and the **`uStrict` harness**. Deleting it would re-open the first,
and it did: the clean cut deleted `uSurf` (§2.81), the movies went black on the Vulkan lane, and
`PK_MOVIE` closes that without the engine's frame ([GL UI renderer](../gui-renderer.html) §25).

#### The change

`tagpu_vk_surf_engine_view(slot, &w, &h)` hands the existing image over. The ordering it needs
already exists: the seam calls `tagpu_vk_surf_prepare` at `tagpu_vk.c:2771` **before**
`tagpu_vk_gui_prepare` at `:2822`, same command buffer, same slot — so the image is uploaded and
barriered into `SHADER_READ_ONLY_OPTIMAL` ahead of the descriptor that names it, and both passes
already wanted that layout. **They do not share a sampler** — each creates its own `s_samp`
(`tagpu_vk_surf.c:303-314`, `tagpu_vk_gui.c:1176-1180`), and an earlier version of this sentence
said they did. It does not matter, and the reason is the one worth keeping: `LAY_FS` reaches
`uSurf` only through `texelFetch`, which **ignores the sampler entirely**, so the borrow is safe
across any sampler configuration rather than by the two happening to agree. The accessor returns `VK_NULL_HANDLE` unless
the slot holds a current frame (`haveSerial`, not merely a live view, because a slot can own a
correctly sized image nothing has been uploaded into yet).

**Deleted with it:** `s_engImg`/`s_engMem`/`s_engView` and their creation, the per-frame `memcpy`
and `copy_rect`, two image barriers, and `s_engHave` (the compose gate asks the same question one
step earlier).

**NOT deleted, and an earlier version of this paragraph said it was** [CORRECTED by the landing
review, which found the same commit re-adding it]: the second bound on the engine frame's
dimensions. `SURF_MAXDIM` 8192 still stands at `tagpu_vk_gui.c:1675`, still downstream of
`TAGPU_SURF_MAXDIM` 4096 at the producer, and therefore still bounds nothing. What went is what it
*protected* — the `mk_image(h.engW, h.engH)` and the `memcpy` sized by them. The relationship is
the trap `tagpu_vk_surf.c:574` names in place, in its second instance, and it is still open.

**And a failure mode moved to where it is tested.** A device that cannot sample `R8_UNORM` now
fails at `tagpu_vk_surf.c:292`, which checks for it by name, instead of through a `mk_image` of
the UI pass's own that never did.

#### Measured

`renderer=vulkan`, the shipped arm set:

| | before (`main`) | after |
|---|---|---|
| shell frame | 640×480, 148 colours, 0 magenta | **identical** |
| in-game frame | 1024×768, 709 colours, 0 magenta | **identical** |
| in-game frame stability, 2 s apart | — | **0 differing pixels** |
| `noeng=` / `skipped=` / fps | — | 0 / 0 / 60.0 |

**The shell is not a pixel oracle** and this landing is where that was measured: it varies against
*itself* by **181–191 px** between consecutive captures, so a diff there cannot see anything
smaller.

**AND NEITHER IS A COLOUR COUNT ACROSS BOOTS** [re-measured 2026-09-18, while re-verifying the
review's fixes]. The row above reads as though `709` were a figure a later run could be held to,
and it is not: the same build, the same `feat-forest` fixture and the same camera to the digit gave
**2 727** colours on one boot and **498** on another, and a bare in-game screen with no fixture gave
**680** where the earlier run recorded 709. What varies is the scene, not the renderer — how much
of the map has been revealed by the time of the capture, and which start position the skirmish
drew. The two `709`s were the same *procedure* run twice, and that is all they are evidence of.
**The count is a liveness check** — a three-digit count means a game is on screen rather than a
lever colour — **and a pixel diff needs a deterministic scene.**

#### Re-verified after the review's fixes

The fixes themselves (the three-way borrow check, `haveSerial` on the re-checked dimension bound)
are meant to be **inert in the normal case**, so the check is an A/B of the two builds over the one
scene that repeats: the shell. Three captures per build, same instance, same window, one build
then the other (not interleaved) — this branch's `ddraw.dll`, then `main`'s rebuilt from `HEAD`:

| | within `main` | within this branch | across the two |
|---|---|---|---|
| differing pixels, shell frame | 182 / 182 / 186 | 183 / 186 / 187 | **180–188** (9 pairs) |
| colours / magenta | 148 / 0 | 148 / 0 | — |

**The cross-build spread sits inside each build's spread against itself**, which is what "inert"
looks like through an instrument with a 181–191 px noise floor. Alongside it, live on the new
build: `noeng=0`, `skipped=0`, `overflows=0`, `fps=60.0`, the in-game frame stable to **0** px, and
**zero** `gui: the surface pass has no image to lend` lines — the new fallback branch is not being
taken, which is the point of it.

#### Not covered

* **The saving is counted from the diff, not measured.** Per frame: one 786 432-byte host memcpy,
  one staging→image copy of the same size, two image barriers, and per-slot staging smaller by the
  same amount; plus one `VkImage` + `VkDeviceMemory` + `VkImageView` no longer allocated at all.
* **`tagpu_ftime` does not resolve it, and it is the wrong instrument.** Four windows each: p50
  **1.218–3.005 ms** before and **1.679–2.823 ms** after — overlapping, no direction. It measures
  **GPU** timestamps, and the larger half of what was removed is a **host** memcpy, which could not
  show there however much it cost. Sizing this properly needs a host-side timer around the upload,
  which this landing did not add.
* **The guard's own behaviour was not re-tested against the case it exists for** — the intro
  Smacker unblanking. The frame being identical everywhere else is consistent with the guard
  working, and is not the same as exercising it.
* **[Closed 2026-09-23: `eng`, `engW`, `engH`, `engPitch`, `strict`, `key`, `guard`, `curEng` and `curOurs` are deleted from `TAGPU_GUIHAND`, and `strict` from the trigger.]** **`h.eng`, `h.engW`, `h.engH`, `h.engPitch` survive in the hand-over** and nothing downstream
  dereferences the pointer any more; it is now only what tells the producer whether to arm `guard`
  and `strict`. Deleting the fields touches the producer/lane seam and was left out of this landing.

### 2.63 A byte flip has no runtime gate to be inert through — landing 10b

**What it is.** `tagpu_owndraw.c` takes over the per-unit composite blit's two structure-shadow
branches so that the native pass can draw a building's cached slant shadow itself. Until this
landing it did that by flipping two `je`s to `jmp`s — `0x4592C6` and `0x45952C`, one byte each,
at `DllMain`, for the life of the process. They are now **detoured** from `0x4592BF` and
`0x459522`, over the `test` and the `je` together, onto a stub that asks a flag first.

**Why it had to change, and it is not about Vulkan.** Every other suppression in this file
already decides per draw, against a word that only a live lane sets:

| suppression | what it asks | where the answer comes from |
|---|---|---|
| the opaque and nano rasterise (`0x459830`, `0x459C70`) | `tagpu_posedraw_live()` | the posed program has linked (`s_state == 1`) and Vulkan does not own the present. **Since 2026-09-23 the predicate is gone: `tagpu_owndraw_classify` never skips a unit, and skips a husk only while `tagpu_native_wrecks_armed()`** |
| the pre-shadow composite wipe (`0x459338`, `0x45958C`, `0x4594DB`) | the same, plus the classifier's own target test | as above. **Deleted 2026-09-23 with its three patches: the gate was the constant 0, so it only replayed the call** |
| `buildfx` (`0x458DD0`) | `tagpu_native_owns_obj` | `s_armed == 1`, written **only** in `tagpu_native_frame` |
| `terrown`, `featown`, `fxown`, `markown` | a `volatile unsigned char` the stub compares | `set_skip(ours-live)` from the pass that paints, with a watchdog. **`cursown` was the fifth and is deleted** (§2.81); `terrown` and `markown` are play defaults (`tagpu_opt.c`); `featown` and `fxown` are not, because a suppressed engine pass holes the reference frame |
| **the structure-shadow pair** (`0x4592BF`, `0x459522`) | a `volatile unsigned char` (`g_ssSkip`) the stub compares | `g_ssSkip = (g_ssTerr \|\| g_ssPass) && g_sshadow`. **`g_ssTerr`** is raised by `tagpu_terrown_set_skip` *before* it publishes its own skip byte — an ordering, so no draw can key-fill under a lowered gate. **`g_ssPass`** is `s_armed == 1 && gl_draws && s_ssSuppress`, published once per frame by the unit pass, and is the half the 8-frame watchdog releases |

And **every one of those answers is produced inside `tagpu_overlay_draw`**, whose only callers
are `render_ogl.c:1632` and `render_vk.c:232`. `render_gdi.c` contains no `tagpu_` call at all.
So on `renderer=gdi` every gated suppression stands down by itself, and the one flip did not:
`tagpu_owndraw.on` was a play default (`tagpu_opt.c`, target `all`), so on the lane this project
calls the stock reference, **every building lost its slant shadow and nothing drew one**.

> **AND THE DEFAULT ITSELF IS GONE SINCE 2026-09-20** (§2.81). `tagpu_owndraw.on` left `s_defs[]`
> with the other `*own` levers, and `tagpu_owndraw_init` begins
> `if (!tagpu_opt_on("tagpu_owndraw.on")) return;` — so unarmed it installs nothing at all: not
> the two rasteriser detours, not `buildfx`, not the three completed-unit shadow sites, not the
> structure-shadow pair. (Armed, it installs all of those but the three completed-unit shadow
> sites since 2026-09-23: they are not patched at all.) The detour below is still the right fix and is what makes the lever safe
> to arm; it is no longer the only thing standing between the gdi lane and a missing shadow.

**The shape of the fix.** The stub is the same shape the other suppressors' stubs have:

```
cmp byte [g_ssSkip],0
jne  TAKE                     ; ours: the engine's skip path, exactly as the flip did
<the engine's own test>       ; copied verbatim, 7 bytes at A, 10 at B
je   TAKE
jmp  <fallthrough>            ; 0x4592C8 / 0x45952E
TAKE:
jmp  <the je's target>        ; 0x459324 / 0x459578
```

`g_ssSkip` is one byte, written by `tagpu_native_frame` on the render thread and read by the two
stubs on the game thread — atomic on x86, no interlock owed. **Its stale directions are not
symmetric**, and that is its safety argument: stale 0 while the pass paints is a DOUBLE shadow
for a frame; stale 1 while it does not is NO shadow.

**So the flag is raised in exactly one place and lowered in nine**, and the first version of this
landing got that backwards. It published from the top of `tagpu_native_frame`, before **nine**
early returns — a frame with no packet, which is the whole shell; a GL program still building; an
atlas that would not allocate; a viewport that failed its sanity bound — so on every one of those
the flag stayed raised with nothing painting, which is the bad direction. The raise now sits after
the last of those guards, at the point the pass has committed to drawing, and each early return
lowers it on the way out. **One write per frame**, so there is no window a game-thread read can
land in: a shell frame writes 0 and only 0, a drawing frame writes 1 and only 1, and the value
changes only when the pass's own state does. (Nine, not eleven: an earlier version of this
paragraph said eleven two sentences after saying nine. Nine publish 0 before the raise -- EIGHT
through `SSHADOW_NONE()` and one, the `s_state == 2` return, through a literal
`tagpu_owndraw_set_structshadow(0, ...)` call, which a `grep -c` of the macro does not see. Nine sit before
the publish; three more sit after it — the `nu == 0 && nfx == 0 && …` nothing-to-draw test, the
`nv == 0 && nhi == 0 && npd == 0 && …` no-geometry test, and the `!gl_draws` hand-over — twelve in
all, no `goto`. Counted by `grep -n 'SSHADOW_NONE\|return;' ` rather than quoted by line, because
every line number in this section was stale one commit after it was written.)

**AND A PUBLISH IS NOT GUARANTEED, WHICH IS WHY THERE IS A WATCHDOG** [the landing review of 10b
found this claim false and pointed at two comments already in the tree that record the same lesson
for the cursor]. This section used to say *"`tagpu_overlay_draw` calls the publisher
unconditionally, so it cannot go quiet while a lane still presents."* It does not: **four gates sit
above the call** at `tagpu_overlay.c:464` — a bad frame ABI (`:306`), **`tagpu_overlay.off`
(`:380`), which is a live lever polled every frame**, an overlay init that failed or was invalidated
by a context change (`:383`), and a level teardown (`:390`). Create `tagpu_overlay.off` mid-session
with the gate raised and the publisher is never called again: the flag freezes raised, the engine's
branch stays suppressed, and nothing paints — the fault this landing exists to remove, reached
through another door. `render_ogl.c:1633` and `render_vk.c:233` both already say this in as many
words, about the cursor, because the same class was found and fixed there.

So `tagpu_owndraw_flush` lowers the gate after **eight** frames without a publish. It is called from
`tagpu_overlay.c:365`, **above three of the four gates** — `tagpu_overlay.off`, the failed init and
the teardown — which is the whole reason it can: it still gets a frame when the publisher does not.
It is **not** above the frame-ABI check at `:306`, and an earlier version of this paragraph and of
the comment in `tagpu_owndraw.c` both counted that one as covered. Nothing is owed for it: both
call sites fill `f.abi` from the same header in the same DLL and pass `&f`, so it cannot fire — but
the claim was the same species as the one this whole section exists to correct, so it is written
down rather than quietly dropped. [Found by the re-review of 10b.]

**The heartbeat is measured against the lane's own frame counter**, and the two lanes count
separately — `render_ogl.c`'s `g_tagpu_frames` and `render_vk.c`'s `s_frames`. A lane switch can
therefore make `frame_counter - g_ssBeat` large and fire the watchdog once. It fires in the safe
direction (it lowers), and the publisher re-raises in the **same** frame, because the flush at
`:365` runs before `tagpu_native_frame` at `:464`. Self-correcting and bounded at one frame; the
unsigned subtraction is otherwise the standard modular idiom and is correct across a counter wrap
(2³² frames is about 2.3 years at 60 fps). `tagpu_fxown.c` solves the identical problem the identical way,
at 90 frames; eight is used here because a missing shadow is a picture the player sees rather than
a counter.

A lane that never presents never calls either of them, which is the `gdi` case: the flag stays at
its initial 0 and the engine draws its own shadows, **by construction rather than by anyone
remembering to switch it off**.

**And a failed rollback is now harmless, which the flip's never was.** If one site installs and
the other does not, `restore_sshadow` puts the first one's bytes back — but even if that
`VirtualProtect` fails and a detour is stranded, `g_sshadow` is 0, so `set_structshadow` computes
`ours && g_sshadow` = 0 and the gate can never be raised: a stranded stub is a pure pass-through.
The old code's failed rollback left `EB` in place, which is permanent suppression. [Pointed out by
the landing review as an improvement the landing had not claimed.] **Both halves of the rollback's
own leak are closed now**: `install_sshadow` frees its stub when the `VirtualProtect` on the engine
bytes fails (the landing review), and it hands the stub back through a `stubOut` parameter so that
undoing A after B refuses frees A's page too (the re-review). The restore puts the engine's bytes
back *before* freeing, so the branch stops pointing at the stub first — free at `DllMain` where no
thread has reached either, but the order costs nothing and a later caller would need it.

**A flip is immune to an incoming branch and a detour is not**, so both stolen ranges were
checked before the code was written. `objdump -d -M intel` over the whole image finds exactly one
branch into either — `0x4594D6 je 0x459522`, which lands on the first byte, our `E9` — and each
of the four addresses searched through the raw image as a little-endian 32-bit datum returns
**0 occurrences**, so no jump table points at one either. Neither branch target consumes incoming
flags (`0x459324` and `0x459578` both open `shr al,0x3`), and neither stub touches `eax` or
`ecx`, which are the registers live across both sites.

**The log line that hid this for a year is corrected too.** `owndraw:`'s arming line ended
*"(engine rasterise skipped for target; writeback must paint it)"* — a fixed string, printed
whether or not anything is ever skipped — and the survey that opened this landing read it as a
report of behaviour. It now says the hooks are installed and that every skip is decided per
draw, and the structure-shadow field reads `HOOKED` rather than `OURS`.

#### Measured

`renderer=vulkan`, the shipped arm set, instance on the `exit-sort` fixture — a Kbot standing on
an ARM lab's own tiles, so **a structure and its slant shadow are in frame**, camera reproduced to
the digit (`[2000, 1200]`, eye `[1616, 806]`).

| | this branch | `main` rebuilt from `HEAD` | cross-build |
|---|---|---|---|
| world frame, below the message log | 0 / 1 / 1 px | 0 / 1 / 1 px | **0–1 px** (9 pairs) |
| whole frame including the log | — | — | 5 182–5 183 px, **all of it the message text** |
| colours | 1 535 | 1 535 | — |

**The cross-build difference over the world is the within-build floor**, so the gated detour
renders exactly what the flip did while the flag is set — which is the regression this landing
could most easily have caused.

**The whole-frame number is a trap worth keeping.** `exit-sort` clears units to place its own,
each death writes a line to the message log, and TA picks the wording at random — *"vermin have
been exterminated"* against *"forces have been obliterated"* — so two boots differ by ~5 200 px of
TEXT with an identical world beneath it. A paired A/B on any fixture that clears units has to
exclude the log band or it is measuring TA's random number generator.

And the gate was watched moving, live, in every direction it has:

| what was done | what the log said |
|---|---|
| the pass arms in a game | `… is SKIPPED (ours live)` |
| `tagpu_native.on` removed | `… is restored`, within one lever poll |
| in the shell, before any game | **nothing** — the gate is never raised there, where the first version of this landing raised it before the first frame |
| Classic++ off, `renderer=vulkan` | **nothing** — no slant painter on that lane, so the gate correctly stays down |
| `tagpu_classicpp.on` armed, same lane | `… is SKIPPED (ours live)` |
| `tagpu_overlay.off` created with the gate raised | `… is restored -- the unit pass stopped publishing`, from the watchdog |

That last row is the one the review's H1 asked for and the one no amount of reading would have
settled: the publisher genuinely stops being called, and the gate genuinely comes down anyway.

#### THE PICTURE, AND THE FOUR ROUNDS OF BEING WRONG ABOUT WHY THERE WASN'T ONE

**There is a picture. It was obtainable all along, on the shipped GL lane, and the argument that
it was not is the single most expensive mistake of this landing** [the fourth review found the
argument false; the A/B below was then run and it is decisive].

**What this section used to say, and why it was wrong.** It claimed that *"the engine blits its
cached slant shadow into the engine's own 8-bit surface, and with `tagpu_terr.on` armed our
terrain pass owns every pixel of the viewport"*, so the restored shadow could only be seen on
`renderer=gdi`, which no `tacli` verb can drive. **The composite does the opposite.** Its
fragment shader is
`if (!cur && int(texelFetch(uSurf, p, 0).r * 255.0 + 0.5) != uKey) discard;` — inside the
viewport **our** fragment is discarded wherever the engine's surface is *not* the key index, so
the engine's pixel wins. The comment thirty lines above it says so in as many words: *"every
OTHER index there is by construction something the engine drew afterwards, so we discard OUR
fragment at those pixels and its own already-drawn frame shows through."* The 0-px result that
started the belief was taken on `renderer=vulkan`, where `keyOn` is −1 because `f->surface_tex`
is 0 until landing 4c, so the engine's surface is not composited at all. It was a Vulkan result
generalised to a lane it does not describe.

**THE A/B, RUN 2026-09-18 ON `renderer=openglcore`.** Instance at 1024×768 with the shipped play
defaults, `scenario load shadow-struct` (ARMCOM, ARMSOLAR, ARMMEX, CORMEX, CORWIN), camera
`[1600, 1600]`, eye `[1216, 1203]`. One capture with the gate up, then `shadows=0` written into
`tagpu_classicpp.cfg` and one with it down — the log records both transitions,
`classicpp: shadows=0(off)` followed by `owndraw: the engine's cached slant shadow is restored`.

| | result |
|---|---|
| differing pixels | **13 396** of 786 432, bounding box y 340–575, x 330–735 |
| dominant colour introduced with the gate DOWN | **(0, 128, 128) — 8 779 px** |
| what it looks like | **four opaque teal blobs stamped on the four structures**, at the 1× position |

So the mechanism is now verified by picture as well as by disassembly, and the gate demonstrably
works in both directions on a live game.

**AND THE PICTURE INVERTS THIS LANDING'S SAFETY ARGUMENT.** The note said gate-down was "the safe
direction — a shadow the engine draws is visible, a shadow nobody draws is silent". What the
engine actually draws inside a key-filled viewport is **not a shadow**. It is the ALP blend at
`0x4B8500` darkening palette 254's cyan into opaque teal — `tagpu_owndraw.c`'s header has said so
since G13k, and this is the first time the two halves of that sentence were put side by side.
A player would report teal blobs as a broken renderer, not as a missing shadow.

**Which means the predicate is still asking the wrong question, for the fourth time.** "Did
anything paint a structure's slant" decides whether a *shadow* appears. It does not decide
whether *garbage* appears, and inside a key-filled viewport the engine's cached slant is always
garbage. The question that decides that is **"is the viewport key-filled"** — which is exactly
`tagpu_terrown_filled()` (`g_terrown_skip && g_filled`), is 0 on `renderer=gdi` by construction
because `render_gdi.c` makes no `tagpu_` call, and is 0 whenever `terr_bail()` hands the ground
back through `tagpu_terrown_set_skip(0)`. The shape that follows from the measurement is

**THE GATE HAS TWO INPUTS NOW, AND THE FIRST ATTEMPT AT THIS PUT THE KEY-FILL ONE A FRAME LATE.**
The first fix was `s_ssSuppress = tagpu_terrown_filled() || (pdReady && …)` — the right question,
asked from the wrong place. The composite reads that same state and acts on it **this** frame,
while anything the unit pass publishes is not read by the game thread until the **next** one. So
on every 0→1 terrown acquisition — level entry, and recovery from any of `terr_bail`'s refusals —
the game thread key-filled and blitted its cached slant under a gate that was still down, and the
composite inverted over it: **up to two frames of exactly the teal the fix was for** [the fifth
review of 10b; the single-frame A/B could not see it].

The gate is therefore two OR'ed inputs, written by different owners:

| input | who writes it | when | its errors |
|---|---|---|---|
| `g_ssTerr` | `tagpu_terrown_set_skip` | **before** it publishes its own skip byte; lowered **after** | none on the raise, by ordering |
| `g_ssPass` | the unit pass, once per frame | one frame behind the painters | a double shadow, or a missing one |

**The raise is an ordering, not a window.** Both stores are on the render thread, x86 does not
reorder stores with stores, so a game thread that sees `g_terrown_skip` set has already seen the
gate raised — and its unit blits come after its own terrain blit. Confirmed in the log, which now
prints `owndraw: … SKIPPED (ours live)` **before** `terrown: engine terrain + fog overlay SKIPPED`
at level entry. The fall is published after the skip, so it is late rather than early: one frame
of a missing shadow, the benign direction. The 8-frame watchdog releases `g_ssPass` only —
releasing `g_ssTerr` would hand the engine a draw the composite is still inverting.

| `shadows=OFF` on `renderer=openglcore` | teal (0,128,128) px |
|---|---|
| before the fix | **8 779** |
| after the first fix | **0** |
| after the two-input rework | **0** |

The 8 741 pixels that still differ between `shadows=SOFT` and `shadows=OFF` after the fix are all
greys — our own Classic++ shadows correctly disappearing, which is what the player asked for. The
log shows the gate now *staying* raised across the transition: `classicpp: shadows=0(off)` with no
`the engine's cached slant shadow is restored` after it.

**The shipped default is unchanged by CONSTRUCTION — and the pixel figure once cited beside that
claim is worthless, which is worth more than the claim it was supporting.** The construction half
holds: the terrown input is an `OR`, so it can only raise the gate where it was already down, and
the log shows it already up on both sides of the shipped default.

The measurement half was cited as *"3 349 px over 154 clusters, mean delta 9.5 — boot-to-boot
variance"*, against a noise floor **nobody had ever measured**. Measuring it: two captures of the
**same build, same fixture, same camera, same configuration**, across two boots, differ by
**421 720 px** — 54 % of the frame — at a mean delta of 3.5, with only 2.5 % of pixels differing
by more than 8. The cause is the Classic++ restorer's centre-out progressive reveal, which is
never at the same point two boots running. **A cross-boot pixel count on this stack therefore
measures nothing**, and the 3 349 figure was luck rather than evidence.

What survives is the **exact colour count**, immune to that noise because it counts one
palette-derived RGB triple and the answer is zero: 8 779 teal pixels before, 0 after, 0 after the
rework. §2.62's cross-boot warning was about colour counts; this is the pixel-count half of the
same trap.

**Why this term is safe rather than merely better.** `tagpu_terrown_filled()` is
`g_terrown_skip && g_filled`, and it is **the same word the composite tests** when it decides to
invert and let the engine's pixels through. So the gate is raised in exactly the frames where the
composite would turn the engine's cached slant into teal — the two agree because they read one
flag, not because two predicates were kept in step. It is 0 on `renderer=gdi` for the same reason
everything else here is (`render_gdi.c` makes no `tagpu_` call), and 0 whenever `terr_bail()`
hands the ground back through `tagpu_terrown_set_skip(0)` — which is precisely when the engine is
drawing the world again and its own slant is the correct thing to show.

**Before the fix, every gate-down state was worse than `main`, not equal to it** — `main` flipped both
`je`s unconditionally, so it never drew the engine's slant anywhere and never showed teal. The
reachable gate-down states on the GL lane with `terrown` live are `shadows=OFF`, the posed program
refused, Classic++ off with the posed program refused, `nterr == 0`, and both frames of every 0→1
transition. **`shadows=OFF` is the one a player can reach on purpose, and it is the worst**: the
player asked for no shadows and gets four teal blobs.

#### Not covered

* **The `renderer=gdi` picture.** Still not taken — `tacli ui` returns *"no UI snapshot
  appeared"* on that lane, so the shell cannot be driven past the main menu (landing 10c). But it
  is no longer the *only* lane where the restored shadow is visible, which is what the section
  above got wrong for four rounds: the GL-lane A/B is the picture, and it was available the whole
  time.
  Making `tacli` drive the gdi lane is its own piece of work and would close this.
* **The two other routes to a picture, and why each failed.** Disarming `tagpu_native.on` to force
  the gate down disarms far more than this gate — `terrown` keeps key-filling with no painter, so
  the frame goes teal — and it left only **63 px** of difference at the lab, too small and too
  occluded to be called proof. The purpose-built `shadow-struct` fixture could not carry a
  cross-boot A/B at all: two boots of it differ in **fog-of-war reveal and starting resources**,
  hundreds of thousands of pixels, swamping anything a shadow could contribute.
* **The double-shadow direction was not forced.** Stale 0 while the pass paints is accepted as
  the safe direction and is one frame wide at most; no fixture makes it happen on purpose.
* **Three returns sit after the raise** (`tagpu_native.c:3441`, `:3713`, `:3811`) and do not lower
  the flag. Two earlier versions of this bullet were wrong about them and the second was wrong in
  the direction that matters, so what each one actually is:
    * **`:3811` is the Vulkan lane's normal exit, every frame** — it closes the
      `if (!gl_draws) { … hand over …; return; }` block at `:3753`. Two earlier versions of this
      sub-bullet were wrong about it, in opposite directions, and the second cost a whole review
      round:
        * *"the Vulkan unit pass paints the shadow"* — **it does not.** No file under
          `tagpu_vk*` contains the word `slant`, and `tagpu_posedraw_slant_set` was pure GL
          with no `pd_record` call. [The line citation that stood here,
          `tagpu_posedraw.c:1230`, was correct when written and points past EOF since landing
          11-5d deleted that function; the file is now 1132 lines. The conclusion is unchanged
          and is now stronger — the function does not exist, so it certainly does not record.]
        * *"Classic++'s cast-shadow map paints it there, through `tagpu_shadow_handover`, which
          is why the predicate asks `tagpu_classicpp_on()`"* — **it does not paint it there
          either**, and this one is provable in four steps rather than argued:
          `tagpu_shadow_handover` needs `s_pubHave`; `s_pubHave = 1` is written in exactly one
          place, `tagpu_shadow_end` (`tagpu_shadow.c:527`), which opens `if (!s_live) return;`;
          `s_live = 1` is written in exactly one place, `tagpu_shadow_begin`
          (`tagpu_shadow.c:403`); and `begin` and `end` have exactly one call site each
          (both in `tagpu_native.c`, at the `if (mapLive)` block's head and its
          `tagpu_shadow_hills(); tagpu_shadow_end();` tail), **both below this return**. So under
          `renderer=vulkan` the handover returns 0 at `tagpu_vk_shadow.c:788` and that pass
          stands down. The Vulkan twin is exercised while the **GL** lane owns the present.
      **Nothing paints a structure's slant shadow on the Vulkan lane today**, so the gate must
      stay down there, and it does — `gl_draws` is a term of the predicate.
    * `:3441` is a genuine *nothing to draw* (`nu == 0 && nfx == 0 && …`).
    * **`:3713` is NOT**, and the bullet that said it was gave a construction argument for a
      configuration fact. It tests **emitted geometry** — `nv == 0 && nhi == 0 && npd == 0 && …` —
      so with the posed program unready `nu` can be large, every structure on screen gathered,
      while `npd` is 0. In the shipped set it is unreachable (`tagpu_mark.on` and `tagpu_terr.on`
      are defaults, so `markOn`/`terrOwned` hold), which is a configuration argument and is stated
      as one.
* **The install's `all` and the gate's `s_armed` are different levers**, and nothing ties them:
  the detours install under **owndraw's** `g_all`, the flag follows **native's** `s_armed`, and the
  unit count follows **native's** `s_type`. A hand-set `tagpu_owndraw.on=all` with
  `tagpu_native.on=armcom` gates on commanders while the detours cover every structure. Not
  reachable from the defaults, which pair the two (`tagpu_opt.c`'s `needs`), and not closed here.
* **THE GATE IS GLOBAL AND THE PAINTING IS PER UNIT**, so every way a single unit can drop out of
  the posed path is a structure that loses its shadow while the gate stays up for the frame. The
  known members, none of them introduced by this landing and none closed by it: a `ModelId` that
  will not bound (below); a type that will not bake (`s_poseNoBake`); a bake with no SLANT range
  at all — the slant counting loop tests `pdix[i] >= 0` but not `count[TAGPU_PB_SLANT] > 0`, where
  the wire loop immediately above it *does* test `count[TAGPU_PB_WIRE] > 0`, so `unit_ok` refuses
  the draw and the unit is still counted; and anything past `MAXU == 2048`. A per-unit gate is not
  possible in the stub — it would have to identify the unit from `ecx` — so this is a named limit
  of the mechanism rather than a bug to fix. [The third review of 10b assembled this list.]
* **A unit whose `ModelId` will not bound** (`tagpu_native.c`, `model_root`'s bound) is refused by
  `tagpu_native_owns_unit`, drawn by the engine, and has no `n2` entry — while the gate, which is
  global, still suppresses its shadow branch.
* **The predicate is an OBSERVATION now, and both guessed versions of it were wrong.** This is
  the single most expensive lesson of the landing, and it took two review rounds:

  | version | what it asked | how it failed |
  |---|---|---|
  | `s_armed == 1` | *is the pass armed* | not the question at all; every other suppressor in `tagpu_owndraw.c` asks the first one |
  | `tagpu_classicpp_on()` **or** `(gl_draws && tagpu_posedraw_live())` | *is a painter configured* | treats the two painters as alternatives, and they are not |
  | `s_armed == 1 && gl_draws && s_ssPainter` | *did anything actually paint one last frame* | — |

  **The two painters are not alternatives: both come out of `pdu[]`.** The cast-shadow map's unit
  casters are the posed depth twin (`tagpu_native.c`, the `if (!pdu[k].castSkip)
  tagpu_posedraw_depth_unit(&pdu[k])` loop) and the slant range is `pdix[i] >= 0` (its counting
  loop and its draw loop). `pdu[]`/`pdix[]` are filled in one place and that place bails first —
  `if (!pdReady) continue;`, where `pdReady = tagpu_posedraw_ready()`. **Cited by symbol rather
  than by line on purpose**: the third review found that this landing's own commit had shifted six
  line references that were correct in its parent, inside the bullets that commit rewrote, so the
  headline proof could not be checked against the tree it shipped with.
  So with the posed program refused (`s_state == 2`, a session latch: a missing entry point, a
  shader that will not link, a uniform block over `PD_BLOCK`), `npd` is 0, **nothing casts into
  the map and the slant range is empty** — and the middle version above raised the gate anyway,
  because `tagpu_classicpp_on()` was true. Every building would have lost its shadow for the
  session, **in the shipped default configuration**, with the engine still drawing the buildings.
  `tagpu_posedraw_ready()` is a **precondition of both painters**, never an alternative to one.
* **So the pass's half of the gate is published by the painters, at `tagpu_native.c`'s
  `s_ssSuppress`**, and the publisher at the top of the next frame reads *and clears* it:
  `s_ssSuppress = pdReady && ((mapLive && nterr > 0) || ((gfx & 4) && !(cpp && !hard)))`, sitting
  immediately after the cast-shadow map block. The key-fill half does **not** go through here —
  see "the gate has two inputs" below, which is the fifth review's finding.
* **`nterr > 0` is there because a CASTER pass running is not a RECEIVER sampling it** — the third
  review's HIGH, and the last of the three rounds' worth of the same mistake. `tagpu_shadow_begin`
  builds the cast-shadow map and never consults the terrain module; a structure's slant lands on
  the *ground*, and the ground is `tagpu_terr`, whose `tagpu_shadow_apply` runs only from
  `tagpu_terr_render`, which is called under `if (nterr)`. So `mapLive == 1 && nterr == 0` is
  reachable — by leaving `terr.on` unarmed (there is no `tagpu_terr.off` lever: `.off` does nothing
  while `.on` exists, so the configuration is the arm file's absence), or by any of `terr_bail`'s
  ~10 refusals, several of them session-long — and in it the map is built, nothing samples it, and at the shipped
  `shadows=SOFT` the slant range is force-skipped as well. Every structure would have lost its
  shadow for as long as that held, while `terr_bail` had handed the ground back to the engine.
  `nterr` is the composite's own predicate, quoted the same way §2.x's `if (nterr)` hand-over
  already quotes it. `mapLive` is `cpp && tagpu_shadow_begin(…)` held in a variable instead of
  tested inline, because the gate has to know whether the map **drew**, not whether it was
  configured — `tagpu_shadow_begin` refuses on four grounds past `tagpu_classicpp_on()`
  (`shadows != SOFT`, the engine's own Shadow bit, `amb >= 1.0`, and its GL init or FBO failing).
  `!(cpp && !hard)` is the slant loop's own `continue` quoted from `:4143` so the two cannot drift.
* **Every stale direction is bounded at TWO of our publishes by construction**, which is what
  neither guessed version had at any width. The clear-on-read is what bounds it: any path that
  does not reach the painters — the nine early returns, the hand-over return, a lane switch, a
  refused map — leaves 0 behind and the next publish lowers the gate. **Two, not one** [the third
  review of 10b corrected this]: the publish sits at the TOP of `tagpu_native_frame`, which runs
  at present time, i.e. *after* the game thread has already blitted that frame's units. So the
  gate the game thread reads for its frame N is the painter observation from N−2. Starting a
  painter costs two frames of double shadow; stopping one costs two frames of none. **The obvious
  follow-up is to publish from below the painters instead** — which would halve it, make
  `s_ssSuppress` a local rather than a static, and drop the `gl_draws` term, because the hand-over
  return would lower the gate by itself. It was *not* done in this landing: moving where a
  cross-thread flag is written is the class this stack fails at silently, three review rounds had
  already found a real defect in these twenty lines, and two frames is a transition flicker rather
  than a durable fault. Named here so the next landing can take it deliberately.
* **And the bound is in OUR publishes, not in engine frames.** If the render lane runs behind the
  game thread, more than two game frames can pass between publishes, and the 8-frame watchdog
  counts the lane's frames too. `gl_draws` is ANDed in separately because it is read fresh
  from `tagpu_vk_owns_present()` this frame while `s_ssSuppress` is last frame's, so a GL→Vulkan
  switch cannot carry a raised frame across the seam. This is the same *observe, don't predict*
  shape `tagpu_cursown_publish` / `tagpu_gui_cursor_drew_take` used for the cursor (both gone:
  the first with `cursown` in §2.81, the second on 2026-09-23).
* **[HISTORY — overturned by the measurement above; kept because the reasoning is the record.]
  Classic++ on with `shadows=OFF` used to leave the gate down and the engine to keep its slant.** Both
  terms are false there (the map wants SOFT; the slant loop's `cpp && !hard` skips it). It is the
  safe direction — a shadow the engine draws is visible, a shadow nobody draws is silent. It is
  *not* symmetric with the silhouette, which the composite-wipe detours at
  `0x459338`/`0x45958C`/`0x4594DB` (not patched since 2026-09-23) did suppress in that configuration; the asymmetry is deliberate
  and is recorded here rather than smoothed over.
* **[HISTORY — the trade described here is no longer taken; the gate is raised in these states
  now.] Every gate-down state TRADES A SILENT FAULT FOR A VISIBLE ARTEFACT, and an earlier version of
  the bullet above called it "no regression", which is false** [the third review of 10b]. `main`
  flipped both `je`s to `jmp`s unconditionally at `DllMain`, so `main` drew **no** engine slant in
  **any** configuration. This landing re-enables the engine's cached slant wherever the gate is
  down — and inside a key-filled viewport (`tagpu_terrown.on` is a play default) the ALP blend at
  `0x4B8500` darkens palette 254's cyan into **an opaque teal silhouette at the 1× position
  whatever the zoom**, which `tagpu_owndraw.c`'s own header has described since G13k. The
  reachable cases are `shadows=OFF`, `shadows=SOFT` with the posed program refused, Classic++ off
  with the posed program refused, and both frames of every 0→1 transition. This is still the right
  trade by this landing's own ranking — visible beats silent, and a player can report teal —
  but it is a trade, not a preservation, and the note now says so.
* **The 1→0 transition costs one frame of no shadow**, symmetrically with the 0→1 frame of double
  shadow: the engine's surface for a frame was painted by the game thread before the render thread
  lowered the flag. Both are one frame wide; the asymmetry the design rests on is about *durable*
  states, not about single frames.
