# Vulkan only — removing the GL backend, planned

**Written** 2026-09-16, before any of it is built, out of an interview with the owner. Nothing
here is measured: every figure is a target and every mechanism is a claim until a landing
carries it. Like [g19f-plan](g19f-plan.html) this file is folded into `gpu-status.md` and
`roadmap.md` when the work closes, and deleted rather than left to rot.

**It repeals a standing constraint.** `roadmap.md` Phase G constraint 4 reads *"The GL backend
is not removed and not regressed."* The owner struck it on 2026-09-16 with the reason recorded
here: GL is wanted only as a reference for the original game, and that reference is going too.
Reach is explicitly not a consideration — the decision was put twice and reaffirmed.

The property that constraint bought goes with it: Phase G could be abandoned after any gate
without debt, because the GL renderer was always still there. After the first deletion that is
no longer true.

## What the end state is

`ddraw.dll` carries **two** renderer backends and creates no OpenGL context on any path:

    renderer=vulkan   the patch                (default, shipped)
    renderer=gdi      the original game        (the reference)

Deleted: `render_ogl.c` (2 013 lines), `render_d3d9.c` (742), `opengl_utils.c`,
`openglshader.h`, `render_ogl.h`, `tagpu_abshot.c`, every `tagpu_*` GL draw, and
`tagpu_overlay.off`.

**`src/IDirect3D*.c` STAY.** They are DirectDraw's COM interface surface — what TA gets from
`QueryInterface` on the DirectDraw object — and nothing in them references `d3d9_render_main`.
Deleting the d3d9 *renderer* must not touch them. Stated here because the two are one word apart
and the mistake is silent.

## The decisions, and what forced each

### The lane is a renderer backend, not a lever

`render_vk.c` with `DWORD WINAPI vk_render_main(void)`, dispatched from `dd.c:1982` beside the
other two. `tagpu_vk.on` and `tagpu_vk.off` retire into `renderer=`.

Two things fall out of owning the thread. **The bring-up can be synchronous** at the top of the
thread proc: the five-state machine (`OFF`/`STARTING`/`READY`/`FAILED`/`ZOMBIE`) and its worker
exist because `vkCreateInstance` loads an ICD and that must not happen from `DllMain` or
mid-present — on our own thread before the loop, neither applies. And **route D's window
collapses**: `tagpu_vk.c:540` creates a top-level `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW` that
tracks the owner's client rect and answers `HTTRANSPARENT`, all of it there because *"two
backends must not both present to one window in one frame"* (`tagpu_vk.h`). With one backend it
presents into `g_ddraw.hwnd` like `gdi` does, which also retires both of G19a's uncovered items
— the alt-enter cycle and the click-through with the shield off.

Constraint 3 survives intact: no pass may know a window exists, and `tagpu_vk_pass.h` is still
the whole of what one is handed. *Which* window the seam creates was never the constraint.

### The unsuffixed modules keep their gather half; `_vk` keeps its name

The Vulkan lane is not a second implementation of anything. `tagpu_vk_terr.h` states it: *"The GL
edition is `tagpu_terr.c` and stays the source of the instances, the uniforms, the texels and the
shader."* The hand-over is pure CPU data — instance shorts, uniform floats, texel bytes with
serials, explicitly *"as bytes rather than as GL names — a second backend cannot read a GL
texture"* (`tagpu_terr.h:192`) — and `terr_publish` runs after the GL draw without depending on
it (`tagpu_terr.c:1605`). So deleting GL is **code motion**: the draw goes, the gather and the
publish stay.

That leaves each pass as two files on opposite sides of a build-enforced line:

| file | rule |
|---|---|
| `tagpu_terr.c` | on `thread-split.allow` as `fenced` — *"the tile set, the tile map and the height byte of every cell, per map under the fence"* |
| `tagpu_vk_terr.c` | `tagpu_vk_pass.h`: *"A PASS READS NO ENGINE STATE … so no pass file may go on thread-split.allow"* |

`tools/thread-split-check.sh` enforces it on every build, and merging the pair to drop the prefix
would put engine-memory-naming code in a pass's translation unit. So **`_vk` is kept and
re-documented**: after GL leaves it stops meaning "the Vulkan variant" and starts meaning *the
half that owns no engine memory, knows no window, and lifts into another process unchanged*.
That set is exactly what constraint 3 protects.

## What is actually unported

The audit in landing 1 is what settles this; the following is what reading the tree says, and it
is already more than the gate rows imply.

**Passes with a Vulkan twin:** scaffold, unit, shadow, terrain, features, effects, GUI, fps.

**Passes with none:**

* **`tagpu_mark.c`** — health bars, cursor, band box, group digits. 19 GL binds, drawn at
  `tagpu_native.c:4294`. **Its shaders are already translated** — `inc/spirv/tagpu_mark.spv.h`
  exists; G19c did it and only the pass file was never written.
* **`tagpu_restoreglsl.c`** — the Classic++ restorer. The one module with no SPIR-V, excluded
  from G19c because its GLSL is not fixed at build time.

**And two stand-downs that make the gap wider than "two passes".** `tagpu_vk_unit.c:1316`:

    if (h.otherDraws > 0) { ... goto standdown; }

`otherDraws` counts units the GL twin drew that the hand-over does not carry — *"a build ghost,
a unit past `TAGPU_PD_MAXHAND`"* (`inc/tagpu_posedraw.h:216`). When it is non-zero the unit pass
draws **nothing at all**. So on the Vulkan lane today:

* placing a building (a play default: `tagpu_ghost.on` needs `tagpu_native.on`) blanks every
  posed unit on screen;
* so does any frame with more than **512** posed units — `TAGPU_PD_MAXHAND`, a deliberate
  ceiling under `MAXU`'s 2048, chosen because each unit costs a 14 336-byte uniform window per
  frame slot.

Neither is visible from the configuration Phase G was measured in.

## The blind spot, which is the reason landing 1 is not code

Every Phase G figure was taken under `tagpu_defaults.off` + `ss=1` + `gui.on=mmbase`, on one GPU
under Wine. Three separate gaps found while planning this all hid in that one configuration:

* **`ss=2` has no target.** `gpu-status.md:4396` already says *"The A/B needs `ss=1` anyway: the
  GL capture is the supersampled FBO and the Vulkan one is the client rect."* The shipped default
  is `ss=2` (`tagpu_native.c:50`, `:2531`) and it is a menu row (`tagpu_menu.c:184`). The Vulkan
  lane creates no offscreen world image; the only trace of `ss` on that side is a `uSS` uniform
  at `tagpu_vk_unit.c:1159`, which scales line widths, not a target.
* **`tagpu_gui.off` leaves no picture.** The seam clears to the lever colour
  (`tagpu_vk.c:2370`) and TA's own surface reaches the frame only through the GUI pass's
  hand-over. On GL the fork uploads and composites it before any pass runs, which is why turning
  our UI layer off works there.
* **The build ghost and the 512 cap**, above.

So **landing 1 writes no code**: start the existing lane under `tacli launch --defaults` with
`ss` on and the full play set, shell and in game, and write the list of what it does not do into
`gpu-status.md`. The landing order below is a draft until that list exists.

## The three oracles

Deleting the GL twins destroys the instrument every Phase G figure was taken with, so each claim
gets the oracle that is valid for it and no other:

| claim | oracle | valid when |
|---|---|---|
| a pass was ported without changing pixels | previous-build A/B, `tools/vk-ab.py`, 0 px | op coverage held fixed — the mechanism that measured constraint 4 against `afceba5`. **It earned its place on gate 2**: an in-process control said a 5-px difference was pre-existing and the previous-build A/B proved it was not |
| we draw a thing the engine draws, identically | `uiwalk --strict`, 0 diff / 0 holes | under `norestore`; it diffs against the engine's own *indexed* surface |
| restored art is right | `tascene uidiff`'s Q2 bar, the `--restore` walk | Classic++ on |

`tagpu_abshot.c` (the GL half) dies with GL; `tagpu_vk_shot.c` and `vk-ab.py` survive unchanged,
because the latter only diffs two files and does not care what made them.

**THE A/B AND THE SHIPPED CONFIGURATION CAN NEVER BE THE SAME RUN** [MEASURED 2026-09-16, and
this plan implied otherwise]. The A/B requires `ss=1` — the pass refuses outright otherwise, and
says so: *"the A/B needs ss=1 (the GL capture is the supersampled FBO)"* — while the patch ships
`ss=2`. It also requires exactly one pass drawing, where the shipped set has eighteen. So "does
it draw in the configuration players get" and "do the two lanes agree" are permanently separate
measurements, and a landing states both or says which it skipped.

**The known weakness, recorded rather than solved:** 0 px per step does not bound drift across
many steps. An absolute bar would, and committing golden captures was rejected — the hooks refuse
binaries outside `.publish-allow` and every intended change would mean reviewing images.

## The landings

One pass per landing, each with its own measurement, never as one drop — the lane's standing
rule. **Each landing deletes its own pass's GL draw in the same diff**, once the figure is banked.

**Landing 1 RAN 2026-09-16 and reordered the rest.** [gpu-status](gpu-status.html) §2.35 is the
result: in the shipped configuration the Vulkan lane draws **the UI and nothing else** — 630 589
of 786 432 px at 1024x768 are the clear colour. Two independent causes, both of which block every
world pixel, and neither of which was in the draft below:

* the **Classic++ restored atlases have no CPU mirror**, so `unit`, `terr`, `feat` and `fx` each
  stand down — and `classicpp` is a play default;
* the **cast-shadow map has casters this lane cannot draw** (*"the native 3DO stream and the
  replacement meshes are still to port"*), and `unit` and `terr` stand down with it.

So the two things that were landings 5 and "whatever landing 1 says" are the two things in front
of everything else. The order below is the corrected one.

1. **The shipped-configuration audit.** ✓ done 2026-09-16, no code,
   [gpu-status](gpu-status.html) §2.35.
2. **The restored-atlas CPU mirror.** ✓ **three of four passes done 2026-09-16**,
   [gpu-status](gpu-status.html) §2.36: features **0 px**, effects **0 px**, terrain **0 px**
   indexed and **5 px of 786 432 at worst channel 1** restored — no regression, established
   against `bfbe8b6`'s own DLL, and inside the bar already accepted for restored art. It needed
   a new read-back helper for terrain (`tagpu_gl_rgba_readback`, in `tagpu_gaf.c` where the entry
   points are resolved) because terrain's twin is painted on the GPU rather than uploaded.
   **The unit pass is deliberately not in it** — it stands down on the caster stream first, so
   its mirror cannot be verified until landing 3, and an unmeasured consumer is not a finished
   unit of work. Its binding 43 stays a placeholder.

   **The lesson worth carrying:** three ordering faults, each hidden behind the one before it,
   all the same shape — *a refusal placed above the code that would satisfy it*. A pass that
   returns 0 skips its own resize and its own upload, so it refuses forever for want of the
   thing the refusal was meant to wait for. And the refusal must test **the view the descriptor
   names**, not the hand-over that says a mirror exists somewhere, or a failed resize draws R8
   through an RGBA sampler instead of standing down.

   **And the second lesson, from the review: 0 px is not a clean bill of health.** Five findings,
   all five real. Two of them — a heap overflow in the mirror's growth test and a 32 MB-a-frame
   re-upload that never stopped — were invisible to every capture above, because both need a map
   change and the A/B fixtures are single-map by construction. One is about a *serial*: what was
   stored was the rows we SENT while what was compared was the rows PUBLISHED, so the two could
   never be equal and the upload ran every frame. Every remaining landing in this plan hands
   bytes across the seam with a serial; that is the class to read for each time.
3. **The caster stream** — so the cast-shadow map can be drawn on this side of the seam and the
   passes that sample it stop standing down. **Landing 3a done 2026-09-16**
   ([gpu-status](gpu-status.html) §2.37); **3b done 2026-09-17** (§2.38). The census now closes
   with a replacement mesh on screen, which is this landing's whole exit condition, and the
   passes that sample the map draw again.

   **The row as filed named the wrong two things, and the measurement is how that came out.**
   It said *"the native 3DO stream and the replacement meshes"*:

   * **The native 3DO stream cannot occur.** `tagpu_shadow_unit` has one call site and it sits
     behind `firstv[i+1] == firstv[i]`; `nv` is 0 for the whole of `tagpu_native.c`'s unit loop
     since G16 step 8 made the posed program the path. Nothing to port. Marked where it stands;
     deletion is landing 11.
   * **What was blocking every world pass was not a caster at all** — it was the UNIT ATLAS'S
     RESTORED MIRROR, the half landing 2 deferred. The unit pass stood down on it several checks
     before it reached its casters, so `tagpu_vk_unit_casters()` answered 0 whatever the casters
     were, the census refused every frame with a unit on it, and the shadow map and the terrain
     stood down behind it. **That is landing 3a**, and with it the census closes on its own for
     stock art: four posed units, soft shadows on, no refusal on any pass, the map drawn.
     Measured: the build before it drew **no picture at all** on that fixture; this one is
     **0 px of 786 432 at 1024×768 and 0 px of 307 200 at 640×480**, with the A/B taken at
     `aniso=1` — the owner's stated substitution, because anisotropic sample placement is
     implementation-defined and the two APIs differ there by up to 9 levels on 27 % of unit
     pixels. Play keeps 4× on both lanes.
   * **3b is the replacement meshes**, and they are not a hires-install curiosity: a tacli
     instance ships `hires/armpw.glb` active, so the census refuses **1** caster with one Peewee
     on screen and **16** on a 257-unit crowd.

     **Sized 2026-09-17, and it is smaller than it looks.** What the map needs is
     `tagpu_hires_depth`, not the whole PBR body pass: positions and the per-vertex piece index,
     the per-group `first`/`count`/`base`/`cutoff` from `TAGPU_HGROUP`, and the per-unit pose,
     anchor, yaw, enc and cast that `TAGPU_HUNIT` already carries. The SPIR-V exists
     (`inc/spirv/tagpu_hires_draw.spv.h`, VS and FS).

     **CORRECTION, 2026-09-17: the vertices are NOT there to be read.** The sizing above said
     they came "out of `tagpu_hires.c`'s static buffer". They do not —
     `tagpu_hires_vao` frees `m->v` the moment the VBO is uploaded, with the comment *"the vertex
     data is the GPU's now"* (named by function rather than by line: gate 3b moved it). A Vulkan pass cannot read a GL buffer, so
     3b needs a **retained CPU copy**, armed the way the restored mirrors are, and that makes it
     a producer change and not only a consumer one. It is the same shape for the third gate
     running — landing 2 read atlases back, 3a read the unit atlas back, 3b keeps vertices — and
     it is the cheapest of the three: `HVSTRIDE` is 9 floats a vertex and the depth path needs
     only three of the four attributes for its arithmetic (position, uv for the cutout, piece
     index; the normal is the body pass's) — though the pipeline still binds all four, because the
     vertex shader is the body's and reads `aNrm` on every path. **Checked, not assumed** — the free is on the line after the group loop's
     texture uploads.
     **The albedo textures are the part that can be deferred.** The depth path samples the albedo
     only for the alpha cutout — `if (uCutoff >= 0.0 && tex.a * uBase.a < uCutoff) discard;` one
     line above the depth early-out — and every material of the shipped `armpw.glb` is
     **alphaMode OPAQUE** (checked in the file: 6 materials, none MASK or BLEND), so `uCutoff` is
     negative for every group and the sampled value is discarded. A 1×1 white stand-in satisfies
     the descriptor, which is what `tagpu_hires.c` already hands a material with no texture.
     So 3b ports geometry and poses, and **refuses any frame in which a group with
     `cutoff >= 0` is drawn** — the same stand-down discipline as everywhere else, and the thing
     that makes the deferral honest rather than a hole.

     **DONE, and it found a defect that belongs to no gate on this list.** Measuring 3b on the
     fixture that could actually see a caster — `crowd-static`, 257 units — turned up **65 px of
     209 814 differing with everything this plan has built turned OFF**: shadows off and the
     restored atlas off, so the caster pass is never reached at all. (Parking the mesh as well is
     a separate control, 164 px at `assets=0`; §2.38 has every row.) All 65 lie on a GL colour edge and 26 carry a
     neighbouring GL pixel's exact value, so it is the two rasterisers disagreeing about which
     triangle owns a pixel an edge passes through. **It has been invisible since G19e because
     every A/B on this plan has been taken on a four-unit fixture**, where no edge lands on a
     sample point. It is 0.031 % of drawn pixels and symmetric, it is characterised but NOT
     traced, and it needs a landing of its own — see §2.38. Nothing below should be measured on a
     small fixture again.

   **The lesson worth carrying, and every remaining landing in this plan is exposed to it.**
   Landing 2's review said a serial is the class to read for each time; 3a's re-review says the
   same about two more, because it found both again one atlas down:

   * **What a "rows" number MEANS on each side of the seam.** 3a stored the rows it *sent* where
     it needed the rows the mirror *covered* — the same mistake landing 2 made with the opposite
     sign, and the symptom is the opposite too: landing 2 uploaded every frame for ever, 3a never
     used its partial path. Neither shows in a capture. Every landing left hands rows across.
   * **A DIMENSION THAT CAN MOVE IS AN IMAGE LIFETIME PROBLEM.** 3a would have rebuilt its image
     when the published mip depth changed, which is `kill_image` on an image other slots'
     submitted command buffers still name — gate 2's confirmed use-after-free in a second place.
     The fix is the one to reach for again: make the moving value *refuse* at the producer so the
     rebuild is unreachable, rather than making the rebuild safe. A pass that cannot reproduce a
     frame draws nothing; it does not draw a smaller version of it.

   And the third is not about the code: **the docs commit had to be rebuilt twice**, once because
   a fixture was wrong before the code was, and once because a character-offset splice left
   `gpu-status.md` carrying three sections twice — including a superseded copy of the very
   section being corrected. The wiki build is what catches it, and it is one command.

   **Two lessons worth carrying.** A gate's row names a mechanism, and the mechanism is a
   hypothesis: measuring the census first cost three runs and saved porting a stream that cannot
   draw, while the thing that actually had to be built was sitting in the previous gate's
   *Not covered*.

   And **a 0 px from a fixture that never took the branch is worse than no measurement, because
   it is reported as a pass.** The restorer only paints when `fx|sfx|feat|terr|mark` is armed, so
   a unit A/B armed with `native.on` alone measures the FALLBACK and calls it parity — landing
   3a's first version did exactly that and was wrong in four independent ways behind it. Before
   quoting a restored-art figure, check the twin actually painted: `tagpu_restoredump.on`, or
   `cmp` the GL captures from `assets=1` and `assets=0`. Gate 2's feature and effects figures
   carry this caveat and now it is known why.
**THE ORDER OF 4 AND 5-7 IS WRONG AS FILED, AND THE WORK FOUND IT [2026-09-17].** Landing 4
deletes Route D. Landing 5 says *"the GL twin is the oracle"*, and landings 6 and 7 need the same
instrument. **They cannot have it once 4 has run**, and this is a property of the code rather than
a preference: every pass draws inside `tagpu_vk.c`'s `if (s_vk.rp && s_vk.fb[idx])`, that
framebuffer comes from `vkAcquireNextImageKHR`, and `tagpu_vk_shot.c` captures a **presented**
image. So the Vulkan lane cannot render one pixel without a swapchain — and while the GL backend
owns `g_ddraw.hwnd`, Route D's own window is the only place that swapchain can come from. Delete
it and a same-frame GL-vs-Vulkan A/B stops being expressible.

The oracle table above is not a way out. Its previous-build A/B answers *"this landing changed
nothing"*, which is the right question for a port of something already verified and the wrong one
for a pass being ported for the first time: it would compare a new Vulkan pass against a previous
build that did not draw it.

**So the order is 5, 5b, 6, 7, then 4, then 8-11** — the exit conditions are untouched and no landing
changes shape; what moves is which of them runs first. 8, 9 and 10 are unaffected either way
because their oracle is `strict`, which diffs against the engine's own surface and never needed
the GL twin. Doing 4 first would have cost the instrument three landings still need, and the cost
would not have shown up as a failure — it would have shown up as three landings with no oracle.

4. **`render_vk.c`** — the fourth backend, `renderer=vulkan`, present into `g_ddraw.hwnd`, the
   offscreen world target at `ss×` with its resolve, TA's surface uploaded by the backend instead
   of by the GUI pass. Route D's window, `tagpu_vk_wndproc`, `WM_TAGPU_VK` and the geometry
   tracking are deleted here.

   **NEXT, AND ITS PREREQUISITES ARE MET [checked 2026-09-17, after 7e-2 landed].** The corrected
   order is 5, 5b, 6, 7, then 4; landing 5 is done (§2.40), 6 is done (§2.41) and 7 closed with
   7e-2, so every landing that needed the same-frame two-lane oracle has had it. Three facts worth
   having before starting, because each took a session's reading to establish:

   * **Its own oracle survives the deletion, and it is a comparison of two BUILDS rather than of
     two lanes.** `tagpu_abshot.c` — the GL half — dies with GL, so the same-run pair does too;
     what survives is `tagpu_vk_shot.c` writing `tagpu_<pass>_vk.ppm` and `vk-ab.py`'s file-to-file
     mode, which "only diffs two files and does not care what made them". So landing 4 is measured
     by capturing each pass's `_vk.ppm` from **`0b5e06d`** (this plan's reference build, `main` as
     7e-2 landed it) and again from the new build, one pass armed at a time, and diffing the pair.
     Both DLLs run in ONE instance under `tacli launch --keep-dll`. **No baseline needs hoarding
     before the deletion** — the reference is a git ref.
   * **`ss=1`, and one `tagpu_<pass>.ab` at a time.** The lane refuses to capture when more than
     one pass drew into the frame and says so in the log, so a contaminated pair is never written.
     The eight capture names are `tagpu_{fps,scaffold,feat,terr,fx,mark,posedraw,gui}_vk.ppm`.
   * **The shipped configuration and the A/B can never be the same run** (measured 2026-09-16):
     the A/B needs `ss=1` and one pass, the patch ships `ss=2` and eighteen. "Does it draw for
     players" and "do the two agree" stay separate questions, and landing 4 owes an answer to both.

   **AND ITS PRESENTATION ROUTE IS MEASURED, NOT ASSUMED [2026-09-17].** Landing 4 presents into
   `g_ddraw.hwnd`, which is the window `vkcoexist.c` route A calls *"API ok, pixels dead"* — so the
   route this landing needs looked, on the face of it, like the one the phase already rejected. It
   is not: route A's dead pixels were **GL's**, and its Vulkan half presented 10 of 10 frames on
   that very window. Landing 4 deletes the half that failed. Rather than argue that, two routes
   were added to the probe and run (roadmap §G19a has the tables):

   * **Route E — Vulkan alone, no GL context and no pixel format ever.** `98.5 % magenta, VULKAN
     REACHES THE SCREEN`, re-run and identical, against a GL control and a GDI control at the same
     98.5 % (the rest is the window border). The fork already arrives in this configuration:
     `dd.c:1524` gates `SetPixelFormat` on `g_ddraw.renderer == ogl_render_main`, so a
     `renderer=vulkan` backend reaches the game window with an untouched HDC and needs no change
     there.
   * **Route F — and then GDI on the same window.** `98.5 % green, GDI REACHES THE SCREEN`. **So
     the fallback survives the surface**, and `render_vk.c` may hand the session to
     `gdi_render_main` at any point, exactly as `ogl_render_main` does when GL will not come up.
     Had this read low, the fallback would have had to be taken *before*
     `vkCreateWin32SurfaceKHR` was ever called and any failure after it would have been terminal
     and had to say so. Either shape is an ordering rather than a heuristic; the measurement says
     which one, and it is the permissive one. **winevulkan's `HWND` takeover is specific to GL's
     drawable, not to the window.**
   * **One lane is cheaper than two, and that is all the VA numbers support.** GL alone 39.2 MB
     committed peak (all four runs), Vulkan alone 37.7-39.5, both 40.6-43.1; largest free block
     490.9 MB in all twelve runs. The two-lane minimum is above the one-lane maximum, so the
     saving is real — but the ranges for the two single lanes overlap, so *"Vulkan is cheaper than
     GL"* is **not** a claim this supports, and the single run that suggested it did not survive
     the repeat. And it is a 320x240 clear-only window: these bound the BRING-UP footprint, not
     the running one.

   **IT IS FOUR LANDINGS, AND THE WORK FOUND THE SEAMS [2026-09-17, written by 4a].** Filed as one
   row; reading the tree for it showed four parts, each along a seam the code already has and each
   with a measurement of its own. The gate's exit condition is untouched — what follows is what the
   work showed about its shape.

   * **4a — the backend exists and presents. LANDED 2026-09-17.** `render_vk.c`, `vk_render_main`,
     the `dd.c` dispatch, and `tagpu_vk_own_present()` putting the surface on the window the caller
     names. Route D still standing. [gpu-status](gpu-status.html) §2.48 is the write-up;
     the result is `our window 00020058 over 00020058 … route E`, an X grab of the **game's** window
     reading 307 200 of 307 200 px at the lane's clear colour, and a `renderer=openglcore` +
     `tagpu_vk.on` control still creating route D's window and still rendering a 148-colour picture.
     It costs **23.4 MB** of committed peak in the real process against the probe's 2.2 MB, with the
     largest free block unchanged — which is the probe's own caveat holding rather than failing.
   * **4b — the per-frame driver. COMPLETE 2026-09-18 (4b-1, 4b-2, 4b-3).** The gather halves run
     and the GL draws stand down.
     `tagpu_overlay_draw` is the single driver and most of it is API-independent; the four GL-owning
     entry points are `tagpu_scaffold_frame`, `tagpu_native_frame`, `tagpu_gui_present` and
     `tagpu_fps_present`. Two things 4a already knows about it: `TAGPU_FRAME` must be filled BEFORE
     `tagpu_vk_frame` with the same frame number, and `vp_y` takes `viewport.y` **without**
     `opengl_y_align`, which is GL's extra scanline and nothing else's.

     **4b IS THREE LANDINGS, AND THE FOUR ENTRY POINTS ARE NOT FOUR EQUAL JOBS [2026-09-18,
     written by 4b-1, count corrected by 4b-2].** The seam is where a pass's HAND-OVER is published, and it is a property of the
     code rather than of how much work each looks like:

     * **4b-1 — the driver and the two passes whose gather is already separable. LANDED
       2026-09-18.** `tagpu_scaffold_frame` and `tagpu_fps_present` each publish their hand-over
       *after* the GL draw and from the same function, so the gate moves inward with the publish
       hoisted out of the GL block and nothing else changes. Both are measured on both lanes;
       [gpu-status](gpu-status.html) §2.49 is the write-up. It also carries the A/B's arming fix,
       which had to come first — see the next bullet.
     * **4b-2 — the world. LANDED 2026-09-18.** All five world passes gather on
       `renderer=vulkan` and are drawn by their twins: terrain, features, markers and units all
       **0 px and byte-identical** against their two-lane captures, effects 0 px on the same-frame
       two-lane A/B (it draws transient projectiles, so a cross-run comparison is not available to
       it, exactly as it is not to the frame-rate readout). [gpu-status](gpu-status.html) §2.50.
     * **4b-3 — the UI layer. LANDED 2026-09-18.** `tagpu_gui_present` gates its own GL and the
       `gl_draws` variable in `tagpu_overlay.c` is gone, so all four entry points are now called
       unconditionally. Shell, whole frame: **0 px of 307 200** on all three comparisons — the
       same-frame two-lane pair, vulkan-only against that pair's Vulkan half, and vulkan-only
       against its GL half. In game the UI chrome outside the viewport is **0 px of 155 648**;
       the viewport itself cannot be compared across two runs, because with `native.on` off the
       engine rasterises the world into its own primary and two runs are two moments.
       [gpu-status](gpu-status.html) §2.51.

       **The filed description was right about the mechanism and wrong about the risk.** It said
       the record's colour flag is keyed on the GL colour twin, and it is —
       `(restored && t->rgb)` reaching `TAGPU_GUICOL_ON` — but that never mattered: Classic++'s
       restorer does not start on this lane, so `t->rgb` is 0 and both lanes honestly record
       "indexed". What actually cost the landing its rounds was a shape the filing did not
       predict: **three functions that read as pure GL executors and are not**, because they
       publish the hand-over the Vulkan twin runs from (`draw_layer`'s `s_mHand`,
       `sharp_begin`'s `s_sharpOn`/`s_sharpInk` and its two clients' `mir_sdraw` records,
       `sharp_minimap`'s CPU bake into `s_mmPicRgb`). Head-returning them built cleanly, ran
       cleanly and would have handed the twin no UI at all. `tagpu_posedraw_live()` in 4b-2 was
       the first of this shape; 4b-3 found three more in one file. **Grepping for `gl[A-Z]` finds
       the first shape and is blind to this one** — the second grep is for what the function
       publishes.
     **WHAT THAT SPLIT LOOKED LIKE FROM THE OUTSIDE, AND WHERE IT WAS WRONG.** Filed as "each
     world pass publishes its hand-over from **inside** its GL render, so terr, feat, fx, mark and
     posedraw each need the treatment individually" -- true of the first four, and the reason each
     took a commit. It was NOT true of the unit pass, and the mis-sizing cost several rounds:
     `tagpu_posedraw.c`'s 150 GL calls needed **no gate at all**, because every
     `tagpu_posedraw_*` entry point is reached through the composite, which the vulkan lane exits
     before. What was missing there was the RECORD, and the fix was to move the exit BELOW the
     unit gather and call three body-path functions from it. The lesson generalises past this
     landing: before sizing a port by counting GL calls, ask which of them the lane can still
     reach.

     **AND THE PER-PASS A/B HAD TO LEARN TO ARM ITSELF BEFORE ANY PASS COULD STAND DOWN
     [2026-09-18].** The oracle above is right and was not reachable: `tagpu_abshot.h`'s rule is
     that the Vulkan half of an A/B may be claimed only on a GL capture that **reached the disk**,
     and on the vulkan-only lane `tagpu_abshot_end` is not merely refused, it is never called — so
     `wrote` is 0 on every frame and the rule refuses every capture on the only lane that presents.
     The rule exists to stop a Vulkan half being diffed against a STALE `_gl.ppm`, so 4b-1 keeps it
     where both lanes run and establishes the same property by construction where only one does:
     `tagpu_vk_ab_arm` unlinks the target `_vk.ppm` and returns whether it is gone, and the pass
     calls it **in the same statement sequence that latches the claim**. A file that exists
     therefore belongs to this arming, and a target that could not be removed refuses the arming
     rather than risking it.

     **WHERE that unlink lives was 4b-1's own review finding, and it is the plan's lesson too.**
     The first version had it beside the capture, in `vk_present` — which is not on the path from
     the latch: the lane's frame function returns before the present all through the bring-up, on a
     swapchain rebuild and at ST_FAILED. It ran on most frames and was missed on exactly the frames
     where no capture happens. A landing whose subject is *safe by construction* shipped a
     guarantee about timing in its own instrument; the review caught it, and the fix is the
     placement. Measured on three refusal paths with a planted sentinel, including a deterministic
     one where the lane is down and the present is never reached at all.

     **Its own oracle turned out to be better than a two-build one, and that is 4a's doing.**
     Because 4a left route D *unreachable rather than deleted*, one binary answers both
     `renderer=vulkan` and `renderer=openglcore` + `tagpu_vk.on` — so a pass can be measured as a
     **same-build, two-configuration** pair (the two-lane A/B at 0 px, then the vulkan-only
     `_vk.ppm` against that pair's Vulkan half) instead of against `0b5e06d`. That isolates exactly
     what the commit changed, where a two-build diff also carries every other change since the
     reference. The previous-build A/B stays the fallback for anything this shape cannot express.
   * **4c — `ss` and TA's surface.** The offscreen world target at `ss×` with its resolve, and TA's
     own surface uploaded by the backend instead of by the GUI pass. This is the part that closes
     two of the three blind spots landing 1 named: `ss=2` has no target on the Vulkan side, and
     `tagpu_gui.off` leaves no picture because TA's surface reaches the frame only through the GUI
     pass's hand-over.

     **WHAT THE CODE SAYS 4c IS, read out of it after 4b-3 landed [2026-09-18]. Facts first:**

     * **The GL lane's `ss` is two FBOs and a box-downsample, and the numbering is the opposite
       way round from what you would guess** [corrected 2026-09-18 — the first version of this
       bullet had the two swapped, which is exactly the re-derivation it was written to prevent]:
       `s_fbo` + `s_colTex`/`s_depTex` is the **1×** pair, and `s_fbo2` + `s_colTex2`/`s_depTex2`
       is the **`ss×`** one, allocated at `w * ss, h * ss` (`tagpu_native.c:831-842`). The world
       renders into `ss > 1 ? s_fbo2 : s_fbo` with the viewport at `gw * ss, gh * ss` (`:4186`),
       and the resolve is a LINEAR quad from `s_colTex2` into `s_fbo` at `gw, gh` (`:4494-4502`).
     * **The target is the GAME's resolution, not the window's** — "the FBO is game_width x
       game_height (the game's requested mode, from the frame struct)" (`:59`), times `ss`.
     * **`ss` is `s_ss ? 2 : 1`** (`:3266`), and `devres` can raise it to `ceil(k)` up to
       `TAGPU_SS_MAX`, in which case **the resolve is skipped entirely** and the composite reads
       the supersampled buffer directly (`:4493`).
     * **There is a second resolve, of DEPTH**, when `selAt1x`: `x_glBlitFramebuffer` from
       `s_fbo2` to `s_fbo`, `GL_DEPTH_BUFFER_BIT`, `GL_NEAREST` — the only filter a depth blit may
       take — so the selection rects drawn at 1× are still occluded by their own units
       (`:4507-4511`). A Vulkan port needs an answer for this or the rects stop being occluded.
     * **The Vulkan lane has no offscreen world target at all.** One render pass
       (`tagpu_vk.c:1727`), attachments `[swapchain image, depth]`, one framebuffer per swapchain
       image (`:1934`), and every ported pass draws straight into the swapchain image at client
       resolution. The only `ss` on that side is a `uSS` uniform that scales line widths.
     * **The swapchain image's colour attachment is `LOAD_OP_LOAD` from `TRANSFER_DST_OPTIMAL`**,
       because `vkCmdClearColorImage` (`:2747`) fills it with the lever's colour immediately
       before the pass begins. **That clear is the slot TA's surface belongs in** — it is the
       backend's own pre-pass transfer and the exact analogue of the GL lane uploading and
       compositing TA's surface before any pass runs.
     * **The machinery for the resolve already exists, in the wrong owner.** `tagpu_vk_gui.c`
       keeps `s_engImg` (the engine's indexed surface, `VK_FORMAT_R8_UNORM`, `:1977`) and
       `s_palImg` (256×1 RGBA, `:1964`) and uploads both from the GUI hand-over every frame
       (`:1818`, `:1983`). So the second half of 4c is largely an **ownership move**, not new
       machinery: the surface and the palette become the backend's, uploaded before any pass and
       independent of `tagpu_gui.on`, and the GUI twin reads what the backend already has.

     **AND BOTH HALVES ALREADY HAVE THEIR SHADER WRITTEN ON THE GL SIDE, which is the thing
     that de-risks 4c most** — `tools/spirv-gen.py`'s whole rule is that the GL lane's GLSL is
     the one copy and the Vulkan edition is generated from it, so neither half needs a shader
     invented for it:

     * **The `ss` resolve is already generated.** `PROGRAMS` in `tools/spirv-gen.py:160` carries
       `("native_d", "tagpu_native::DVS", "tagpu_native::DFS")`, and `s_dprog` is declared beside
       `s_fbo2`/`s_colTex2`/`s_depTex2` (`tagpu_native.c:357`) — it *is* the 2× → 1× downsample.
       So `inc/spirv/tagpu_native.spv.h` already holds the resolve 4c-2 needs; the Vulkan work is
       the target and the plumbing, not the maths.
     * **TA's surface has one too, in the fork's own header.** `render_ogl.c:260` builds
       `g_ogl.main_program` from `PASSTHROUGH_VERT_SHADER` + `PALETTE_FRAG_SHADER`
       (`inc/openglshader.h:43`, `:60`) — sample the R8 index, then
       `texture(PaletteTexture, vec2(pIndex.r * (255.0/256.0) + (0.5/256.0), 0))`. That pair IS
       the base layer the GL lane draws before anything of ours, which is exactly what 4c-1 has
       to reproduce. It is not in `SOURCES` yet, but `RESTORE_HDR` shows a header can be.
       Known risk: those two are `#version 130` and the generator's transform is documented as
       GL-330-to-Vulkan, so the attribute/binding handling is where this will first bite.

     **The toolchain is present.** `tools/glslang/bin/glslang` is vendored at the pinned 16.6.0
     (`tools/glslang-vendor.json`) in the MAIN checkout — it is gitignored and not on `PATH`, so
     `command -v glslang` says MISSING and means nothing. `tools/spirv-gen.py --check` reports
     *47 shaders, 32 programs, headers current*.

     **WHERE 4c-1's PIECES HAVE TO LIVE, settled by the contracts rather than by preference:**
     [2026-09-18, after the shader landed]:**

     * **Not in the pass.** `tagpu_vk_pass.h` is explicit — *"A PASS READS NO ENGINE STATE. Every
       value it needs arrives as an argument"*, and no pass file may go on
       `tagpu/ddraw/thread-split.allow`. Standing constraint 3 adds that surface, swapchain,
       acquire and present live in `tagpu_vk.c` and nothing else may know a window exists.
     * **But the read itself is not an engine read.** `g_ddraw.primary->surface` is the FORK's
       DirectDraw object, not the game's memory at an absolute address — which is why
       `tagpu_gui_surf.c` already reads it in `mir_finish` and is not on the allow-list. So this
       needs no allow-list entry, and the list stays default-deny and shrinking.
     * **`render_vk.c` is the home**, because it is the mirror of `render_ogl.c` — the file that
       owns `g_ogl.surface_tex_ids` and does exactly this upload for the GL lane. Its own comment
       at `:214-221` already says so: *"It is the engine's own 8-bit frame as an R8 index texture,
       uploaded by the GL backend before any pass runs; the backend taking that upload over is
       4c, and until then `tagpu_terrown.c` and anything else reading it has nothing."*
     * **`f.surface_tex` cannot carry it.** That field is a GL texture NAME and stays 0 on route
       E by contract (0 is what the fork already hands a non-8bpp frame), so the bytes need a
       route of their own.
     * **AND THAT ROUTE IS A GETTER, NOT A HAND-OVER THROUGH THE SEAM.** [Corrected 2026-09-18,
       having first written it the other way round.] Every ported pass has the identical
       signature — `int tagpu_vk_<pass>_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb,
       uint32_t slot)`, checked across fps, mark, feat, terr and scaffold — and carries **no
       data arguments at all**. It PULLS, from a getter on the module that owns the answer:
       `tagpu_scaffold_overlay(&buf, &w, &h, rect, &rows, &ab)`,
       `tagpu_feat_handover(&h, d->frame)`. The seam passes a device, a command buffer and a
       slot, and nothing else — which is what keeps `tagpu_vk_pass.h`'s promise that a pass
       could draw into anyone's image without a line of it changing.
     * **So 4c-1 needs a module that owns the snapshot**, not a parameter chain. `render_vk.c` is
       the lane's loop rather than a data module, and `tagpu_gui_surf.c` owning it is the
       coupling this landing exists to undo — so the snapshot of TA's primary and the palette it
       is resolved through becomes its own small module, taken once per frame from
       `tagpu_overlay_draw` (which both lanes reach, and which runs before `tagpu_vk_frame` in
       the same iteration, so the bottom layer is ready before the passes draw). It needs no
       `thread-split.allow` entry, for the reason two bullets up.
     * **The descriptor layout is already fixed by the generated header:** vertex set 0 binding 0,
       std140, 64 bytes (`mat4 MVPMatrix`); fragment set 0 binding 40 `sampler2D Texture` and
       binding 41 `sampler2D PaletteTexture`; attributes at locations 0/1/2, the numbers
       `ATTR_LOCATIONS` chose — **so the pass's vertex buffer must bind to match**, which is the
       obligation that table's comment names.
     * **The draw goes inside the render pass, not in place of the clear.**
       `vkCmdClearColorImage` stays: it is what covers the letterbox and the frame that has no
       surface to draw. The blit is the first draw after `vkCmdBeginRenderPass`, which is where
       a pipeline can run at all.

     **THE SPLIT IS CONFIRMED BY DOING IT: 4c is two landings.** *4c-1* TA's surface — **LANDED
     2026-09-18**, the bottom layer, and `tagpu_gui.off` on `renderer=vulkan` now renders the
     shell and a live game completely where it showed the lever's flat clear
     ([gpu-status](gpu-status.html) §2.52). *4c-2* the `ss×` target and its resolve — **LANDED
     2026-09-18** ([gpu-status](gpu-status.html) §2.53): the world draws into an offscreen
     colour+depth pair at `gw*ss, gh*ss` and one LINEAR draw composites it into the viewport rect.
     **It closed more than `ss=2`.** The lane's effects and marker passes were refusing the WHOLE
     pass on any frame with line vertices, because the twin draws its lines `ss` px wide and there
     was no `ss` target to put them in — so on the shipped default (`ss` is 2 unless
     `tagpu_ss.off` is there) the effects pass dropped every frame with a laser in it. The landing
     asks the device for `wideLines` and turns both refusals into a range check.
     **Measured:** the terrain A/B on route D at `ss=1` differs on **0 of the 630 719 pixels the
     GL FBO drew**; live at `ss=2` the heartbeat reads `2048x1536 target (1024x768 at ss=2)`.
     **And it found that the world A/Bs stopped being readable when 4c-1 landed** — the GL half is
     the bare world FBO, the Vulkan half is the swapchain image and since 4c-1 that carries TA's
     own frame underneath, so every uncovered pixel differs by construction. `tools/vk-ab.py` now
     reports `on GL ink` / `on GL black` and exits 0 when a run differs only on GL-black pixels.
     Not covered: `selAt1x`, the HUD-scale shift, and GL's two-step resolve (one step here, the
     same filter at k = 1 and GL's own `devres` path otherwise).

     **AND IT FOUND AND FIXED A BREAK 4c-1 HAD LEFT IN THE A/B ITSELF.** Since 4c-1 the Vulkan
     half of a world capture was the swapchain image WITH TA's own frame underneath, while the GL
     half is the bare world FBO — so every uncovered pixel differed, and for a pass that BLENDS so
     did every translucent fragment (the effects pass: 1 396 of its 1 622 GL-ink pixels, median
     max-channel 19, against 226 identical at median 237). 4c-1 verified itself on the screen, so
     nothing caught it. The fix is one line in the seam — `if (draw_surf && nclaim == 0)`, TA's
     frame withheld on exactly the frames an A/B is claimed — and **both world A/Bs are 0 px of
     786 432 again**: terrain 630 719 non-black a side, effects 31 532.

     *4c-3* the capture moves to that target — **LANDED 2026-09-18**
     ([gpu-status](gpu-status.html) §2.54). `tagpu_vk_shot.c` read `s_vk.img[idx]`, the swapchain
     image at the window's client rect, while the GL half has always been the world FBO at
     `gw*ss, gh*ss` — the same size only at `ss = 1` with nothing letterboxed. Four passes refused
     their own A/B whenever `ss != 1` and `posedraw` silently wrote a pair `vk-ab.py` then refused
     by size, so **no figure on this plan had ever been taken at the `ss` the renderer ships with**.
     The world colour image now carries `TRANSFER_SRC`, `tagpu_vk_world_shot` names the slot the
     world render pass was opened on, and the seam reads it for the five world rows of `s_abFiles`.
     **Measured at ss=2, 2048x1536 a side — all five world passes: terrain 0 of 3 145 728
     (2 522 876 ink), features 0 (529 096 ink), units 0 (8 465 ink), effects 0 (10 396 ink) on a
     frame with no line vertices. `ss=1` re-measured on the same build is the 4c-2 figure
     unchanged: terrain 0 of 786 432, 630 719 ink.**

     **AND IT FOUND THE GL HALF WRONG, WHICH IS A FIRST ON THIS PLAN.** On a frame carrying laser
     lines the diff is ~100 px, every one of them a pixel GL left black and none of them a pixel GL
     drew: each Vulkan column of the line holds two ink pixels and each GL column one. The cause is
     already a measurement in `tagpu_native.c:337` — the driver clamps an aliased line's width to 1,
     so `glLineWidth(ss)` draws pixel-identically to `glLineWidth(1)` and the twin's laser resolves
     to *"a half-lit smear, about half the engine's colour"*, which is the defect `selgeom` exists
     for. The Vulkan lane has `wideLines` since 4c-2 and gets its 2 px, which resolves to one fully
     1.0 of a game pixel of coverage per step against GL's 0.5 — the engine's own rule, though
     whether it lands in one game pixel or splits across two depends on the block grid. **Measured
     on the marker pass too** (order lines, SHIFT held, a `patrol`): 34 px of 3 145 728, GL inking
     one column where Vulkan inks two on both axes. **Left alone**: making them agree changes a
     shipped picture and is the owner's call, not a landing's (this plan's escalation reason 1).
     Not covered: `devres` and `k != 1` are untested either way, and the steady-state cost of the
     world target's unconditional `TRANSFER_SRC` (DCC) is named in the code and unmeasured.

   * **4d — the deletion. 4d-1 LANDED 2026-09-18** ([gpu-status](gpu-status.html) §2.55): 152 lines
     in, 370 out. The window class, its window proc, the `WM_TAGPU_VK` create/destroy handshake, the
     `WM_WINDOWPOSCHANGED` follow, `s_vkwnd`, `s_askedWin` and the four teardown sites that existed
     only to order a destroy against a live surface — **and `render_ogl.c`'s own call to
     `tagpu_vk_frame`, which this entry did not mention and which was the GL backend's half of route
     D.** The lane is reached only through `renderer=vulkan` now, which is what makes `s_ownWin`
     true on every frame that gets there. `tagpu_vk_enum_start()` stays in `render_ogl.c`: the GPU
     row is player-facing and must work under the GL backend. `tagpu_vk_wndproc` survives reduced to
     one arm — the OWNER being destroyed, the only fact the window thread has that the render thread
     cannot get for itself, and deleting it would have traded an ordering for "wait for an error".
     **Verified by running it, since nothing is left to A/B with**: `renderer=vulkan`, full play arm
     set, the clear colour left at its magenta default — one window and no popup, a complete frame
     at 640x480 and again at 1024x768 after a mode change (the path the window handshake used to
     live in), **0 magenta pixels** either time, and a clean stop in 0.44 s.
     **4c-3 had to come first and did**: it was the last landing that could widen what the two-lane
     oracle can express, and this is what removed the lane it compared against.
     **Its own review found three instruments the deletion had silently killed** and all three are
     fixed in the landing: `tagpu_ftime` was inert for the whole session on this lane (its poll had
     one caller, in `render_ogl.c`) so the still-open "frame time no worse than GL" clause had no
     instrument at all — after the fix it measures **`vk p50 0.154 ms  p99 0.370 ms`** at 1024x768,
     the first such figure this lane has produced; `tools/uiwalk.py --vk`, the walk the G19f UI
     clause was MET with, armed route D exactly and now refuses with a message; and `tagpu_vk.on`
     under `renderer=openglcore` was still latching six one-way gather mirrors (tens of MB) for a
     consumer that no longer exists, so those now ask `tagpu_vk_owns_present()` instead.
     **4d-2 LANDED 2026-09-18** ([gpu-status](gpu-status.html) §2.56): 101 lines in, 599 out,
     `tagpu_abshot.c` and its header among them. It was already unreachable — the captures sat
     behind `gl_draws`, false under `renderer=vulkan` — and 4d-1 is what made it pointless rather
     than idle. **Every pass already contained the code that survives**: all eight had a second, claim-only
     branch written in 4b-1..4b-3 for the lane with no GL half — spelled `else if (taking)` in five
     and a nested `else` in `mark`, `posedraw` and `gui_surf` — so the deletion is each pass
     collapsing onto it. (`posedraw`'s flag is `s_abClaim`, not `s_abFrame`; its `s_abFrame` was
     write-only and went too.) The Vulkan half and the eight `.ab`
     levers stay, per the decision above. **Verified**: the full arm set renders a complete
     1024x768 frame at 0 magenta of 786 432, and `tagpu_terr.ab` alone writes
     `tagpu_terr_vk.ppm` at 2048x1536.

     **`s_curDrew`'s ordering LANDED 2026-09-18** ([gpu-status](gpu-status.html) §2.57), and with it
     **GATE 4 IS COMPLETE**. The publish now happens AFTER the frame it reports, with
     `cur_drew && tagpu_vk_ui_composited()` — an ordering rather than a move, because the take has to
     stay where it is (taking is what clears the producer's flag) and the publish has to stay on the
     path every iteration reaches. The frames it changes are counted rather than argued:
     `held=1` at the shell and `held=3` after walking into a game, surfaced in the GUI heartbeat's
     `cursown=` field. Three frames a session on which the engine's cursor was suppressed with
     nothing of ours on screen. Not covered: which of the five refusal paths those frames took.

     The original statement of the gap, for the record — the plan assigned it to 4c and none of
     4c-1..4c-3 or 4d-1..4d-2 closed it. `render_vk.c` publishes
     `tagpu_cursown_publish(tagpu_gui_cursor_drew_take())` BEFORE `tagpu_vk_frame`, so it asserts
     "our cursor was drawn" while `tagpu_vk_gui_prepare` can still refuse the frame afterwards; the
     engine's own cursor is then suppressed on a frame that composited no UI. The fix is an
     ORDERING and not a move: the publish's position is argued in place (*"the only point every path
     through the driver reaches"*), so hold the taken value, have the seam report whether
     `tagpu_vk_gui_record` ran, and publish `want && composited` after the frame — every early
     return then yields the truth rather than a stale latch.

     **WHAT 4d TAKES AND WHAT IT LEAVES — decided by the owner 2026-09-18, because the plan scoped
     4d to the window and said nothing about the instrument that dies with it.** Keep
     `tagpu_vk_shot.c` and the eight `.ab` levers; delete only `tagpu_abshot.c`, the GL half. A
     single-lane capture is still how a PPM of the Vulkan frame is taken for a cross-BUILD
     comparison, which is exactly the "Drift" item this plan leans on once the two-lane oracle is
     gone. And explicitly NOT in 4d, although the deletion makes it possible: after it
     `tagpu_vk_own_present()` is unconditional, so the `s_ownWin` latch and every
     `tagpu_vk_owns_present()` test behind landing 4b's stand-downs become constant-true. That is a
     far larger simplification than removing a window, and a one-way landing that sprawls is how
     something nobody meant to lose gets lost. **4a made route D unreachable rather than deleted on
     purpose**, so the control above stays available until then: the same build answers both
     `renderer=vulkan` and `renderer=openglcore`, which is what makes 4b's A/B expressible at all.

   Four parts, not ten: the split is along the thread, the driver, the target and the deletion, and
   each is something that can be run and shown. (4c came apart into three along its own seam — TA's
   surface, the target, and the instrument that can finally read the target — which is the row
   catching up with the work rather than a fourth part.) 4a's own bar is met by the table in §2.48.

   **And the deletion is one-way for the whole plan, not just for this landing.** After it, no
   absolute two-lane comparison is expressible; every later claim rests on a relative bar against a
   previous build, which is the "Drift" item below. Anything wanting an absolute figure should take
   it while `0b5e06d` is still the tip.
5. **`tagpu_vk_mark.c`** — bars, cursor, band box, digits. The SPIR-V exists
   (`inc/spirv/tagpu_mark.spv.h`); the GL twin is the oracle and goes in the same landing.
   **After** 2 and 3, because until then it would draw over a world that is not there.

   **AND IT MUST CARRY A REPLACEMENT FOR THE LEVER IT BREAKS [found 2026-09-17, before starting
   it].** Every restored-art A/B on this plan arms `mark.on`, for a reason that has nothing to do
   with marks: `tagpu_rglsl_step` runs from `tagpu_native.c` only when one of
   `fx|sfx|feat|terr|mark` is armed, and **`mark` is the only one of the five with no Vulkan pass
   of its own** — so it steps the restorer while leaving exactly one pass drawing, which is what
   the capture requires. Porting mark makes all five drawing passes, and the recipe stops
   working: the lane answers *"1 A/B levers claimed this frame and 2 passes drew into it"*.
   The per-pass `tagpu_<x>.off` files are no help, because they disable a pass on BOTH lanes and
   the oracle needs the GL half drawing.

   So landing 5 owes a way to step the restorer that arms no pass — the same shape
   `tagpu_gui_surf.c:2465` already uses to step it when nothing else did. Without it, landings 6
   and 7 inherit a measurement they cannot take, and they are the two landings that most need it.
   This is the *Nothing half-done left behind* rule applied to an instrument rather than to code.
   **Done 2026-09-17, before the port**: `tagpu_rglsl.step`, measured at 0 px with a twin that
   painted (the GL lane's own picture moves 6 200 bytes between `assets=1` and `assets=0`).

   **AND THE PORT ITSELF IS SMALLER THAN GATES 2 AND 3, because nothing needs a new mirror.**
   Checked rather than assumed — every input this pass samples is already CPU-side:

   * **the vertices** are `tagpu_mark.c`'s own arrays (`s_verts`, `s_ordt`, `s_ordl`, `s_ordx`),
     7 floats a vertex — x,y, u,v, wx,wz, colour — uploaded to a stream VBO each frame;
   * **the layer** arrives as `TAGPU_MARKLAYER` — `pix`, `pitch` and a rect — which is already
     CPU bytes rather than a GL name;
   * **the text atlas** is already crossed: `tagpu_text_atlas(&gen)` exists, added in G19d "for a
     backend that is not GL", one coverage byte per texel with a generation that ticks when a
     raster lands;
   * **the palette, fog grid and fog LUT** are crossed already for the passes that sample them.

   So landing 5 is a hand-over and a consumer, with no producer-side read-back to build — the
   first of these gates where that is true. What it does need is the DRAW LIST, because this pass
   is not one draw: order triangles, order lines (at `ss` line width), order labels, bars, digits,
   the post-fog layer and the cursors, each with its own `uText`/`uFog` and the last two with fog
   forced off, in the engine's own order (`0x469BFC` → `0x469CB9` → `0x469CF9` → the build cursor
   after the fog overlay). The list crosses as records, for the same reason the caster record does
   in 3b: a consumer that re-derives which buckets are non-empty can disagree with the pass that
   drew them.

   **MEASURED 2026-09-17, and the blocker was the FIXTURE exactly as this entry predicted.**
   `tagpu_mark_render` returns before it draws when every bucket is empty, so the GL half never
   reaches the disk. Four automated runs ended there. What it needed was not one lever but SIX,
   one per draw kind, and none of them is `mark.on`: a unit of the watched player for the bars;
   `ctrl+<n>` for the group digit (`u->squad` non-zero is the whole gate); `order.on` **plus**
   SHIFT physically held **plus** an order that does not complete — a `patrol`, not a `move` —
   for the order lines; the HOVERED unit at game speed 1 for the marching route dots, which the
   engine draws only at `flag == 1`; the typed `+showranges` cheat for the labels; and a HELD
   drag for the cursors. The recipe is in the `ta-drive` skill and the table is in
   [gpu-status](gpu-status.html) §2.39.

   **The first A/B was a health bar and nothing else, and measured 0 px.** Extending the fixture
   to six of the seven kinds took it to 4 066 px and two real defects came apart: a line pipeline
   that never chained `VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT`, worth **4 900 px** on 436
   segments of route line and range circle; and a text atlas uploaded every frame and **never
   bound**, so every label and digit sampled the layer binding's 1x1 stand-in, never discarded,
   and came out a solid filled quad — **3 891 px**. Fixed, the pass measures **32 px of 786 432**
   with the non-black counts equal at 10 324 a side, and those 32 px are landing 5b's flip.

   **All seven draw kinds are measured.** The band box (held open across the capture) and the
   captured post-fog layer (`mark.on=nocursor`) are **0 px** each — area primitives, whose sample
   points sit on the half-integer grid and so cannot tie the way a zero-width line does.
   **Not covered**: `ss != 1` is refused rather than drawn, so it is a bound and not a gap; zoom
   is untested either way.
#### Landing 5b — THE LANE'S FRAME IS UPSIDE DOWN, and no A/B on this plan could see it

[FOUND BY THE OWNER, LOOKING AT THE ROUTE D WINDOW, 2026-09-17.] Filed here, between 5 and 6,
because 6 and 7 are the last two landings that get the GL twin as an oracle and there is no
sense measuring them through an instrument that is wrong.

**It is not an undocumented fact — it is a documented decision whose blind spot nobody had
reason to look into.** [gpu-status](gpu-status.html) §2.28 says in as many words that *both
halves of the A/B are upside-down pictures of the world, and that is correct*: the GL twin
draws into the world FBO, whose clip +1 is the bottom of the screen, and GL's composite quad
turns it over on the way to the window. `tagpu_vk_fx.c`'s `vp.y = h; vp.height = -h` makes the
Vulkan image match that FBO, and `tagpu_abshot.c` turns the GL rows over so the two line up.
The comparison is honest. What it never covered is that **the ported passes draw straight into
the swapchain image and there is no composite quad on the Vulkan side**, so the lane's own
window shows the FBO orientation — upside down. That went unnoticed because until landing 5 the
Vulkan lane had no picture a human ever looked at.

**What this does and does not invalidate.** Geometry, colour, coverage and draw ordering were
all genuinely compared and every published figure stands as a CONTENT comparison. Two things
fall outside it: the lane's presented picture, and any rasterisation rule whose answer depends
on which way up the viewport is — landing 5's 32 px are the first of the second kind to appear.

**The work**: eight viewport sites (`fps`, `fx`, `mark`, `feat`, `unit`, `scaffold`, `gui`,
`terr`) to a positive height; five scissor rects that are deliberately mirrored to compensate
(`fx_scissor` and its four copies) to the unmirrored rect; `tagpu_abshot.c`'s row order; and
`tagpu_vk_feat.c` item 3, `tagpu_vk_fx.c` items 5 and 6 and §2.28's prose, which are
correct about the FBO they describe and have to be restated once the lane presents upright. `tagpu_vk_shadow.c` is untouched and always was right — its map is an
offscreen texture sampled by UV and its header already says NO Y FLIP. Nothing culls
(`VK_CULL_MODE_NONE` in all thirteen ported pipelines), so the winding argument the flip was
justified by buys nothing.

**Exit condition**: the Route D window is upright for every armed pass, AND every A/B on this
plan is re-run and agrees with the figure already published.

**DONE 2026-09-17** ([gpu-status](gpu-status.html) §2.40). The fix set turned out to be five passes
and not eight: the tree has TWO y conventions and `gui`, `fps` and `scaffold` were already right —
each of those three established on the screen in its own right — the GUI by the two windows
differing on exactly the world viewport and nowhere else, the FPS readout by reading upright in
Route D's own corner, the scaffold by its row tint sloping the same way in both — rather than
inferred from their shaders, which is the inference this landing exists to distrust. Game window vs
Route D: **1 394 px of 786 432**, against 624 824 mirrored. Every A/B re-taken and at parity —
terrain, features, units, effects and the untouched GUI control all **0 px**. Landing 5's 32 px
were this flip and are gone.

Back to the filed list:

6. **The build ghost and the `otherDraws` stand-down** (`tagpu_vk_unit.c`'s `h.otherDraws > 0`).

   **MEASURED 2026-09-17, BEFORE STARTING THE PORT, AND THE COST IS TOTAL.** The row said "not
   reached today — the unit pass refuses on the atlas mirror several checks earlier — so its cost
   is still unknown". Gates 3a and 3b removed that earlier refusal, so it is reached now. With one
   ARMCOM selected and a solar placement open (`one-unit`, `native.on=all wrecks` + `ghost.on` +
   `mark.on`, cursor mode `0x0E` confirmed by peeking `main+0x2CC3`):

   ```
   ghost: curs=6193 queue=0 drawn=6193 nobake=0 trunc=0 alpha=0.40
   vk: unit: the GL twin drew 1 posed unit(s) this hand-over does not carry
             (a build ghost, or past its cap) - nothing drawn while that is true
   ```

   So **for as long as a building placement is open, the Vulkan unit pass draws nothing at all** —
   not the ghost, not the units. `tagpu_ghost.on` is a play default gated on `tagpu_native.on`
   (`tagpu_opt.c`), and placing buildings is most of what a TA player does, so this is the ordinary
   case rather than a corner. It is the largest remaining hole in the lane's world.

   **The gate to know before reproducing this**: the ghost arms on `tagpu_mark_cursor_ours()`, so
   it draws only with the MARKER pass armed — and `markown` installs its detours at DLL attach, so
   `mark.on` written to a running instance opens nothing. Two probe runs were lost to that;
   `ghost: curs=0 drawn=0` with cursor mode already 14 is the signature.

   **DONE 2026-09-17** ([gpu-status](gpu-status.html) §2.41). It was **not** the removed exclusion
   it looked like: `ghost_pass` opens a SECOND `posedraw` window and `s_recording = (s_win++ == 0)`
   meant only the first recorded, so dropping `|| u->ghost` changed nothing at all. Every window
   records now, while the first stays the one that publishes the view and the one the A/B brackets.
   The consumer is one extra pipeline — `s_pipeGhost`, the body pipeline with
   `depthWriteEnable = VK_FALSE`, which is the single bit the twin's `glDepthMask(GL_FALSE)`
   bracket moves. The other two `s_other++` sites stay refusals.

   **A caster the GL depth pass never drew, caught before it shipped**: `ghost_one` memsets its
   record and sets only `alpha` and `ghost`, leaving `castSkip` 0 — harmless while ghosts were
   refused, but `pd_record` computes `casts = (depthOn && !castSkip)`, so carrying them would have
   handed every ghost to the shadow census as a caster the GL map has no silhouette for. An extra
   caster there is a wrong shadow map, not a missing one.

   **AND THE A/B CANNOT BE THIS LANDING'S ORACLE**, for two independent reasons: the ghost arms on
   `tagpu_mark_cursor_ours()` so it needs the marker pass, which then draws the placement square —
   two Vulkan passes, which the capture refuses — and the GL half is blacked and read back around
   the FIRST window while the ghost draws in the second, so it could not contain one anyway. The
   pass declines to claim the pair on a ghost frame rather than report a difference that is the
   instrument's. **Landing 5b's two-window comparison is the oracle instead**, which is the second
   time that method has paid for itself.

   **Measured**: cursor mode `0x0E`, `ghost drawn=11998`, the stand-down gone (**0** occurrences),
   and the GL frame against Route D at **260 px of 786 432** — of which **0 lie inside the ghost's
   own box**. The 260 are on the ARMCOM's body, worst channel 184, the same class as §2.40's 311 px.
   **Not covered**: `ghost: queue=0` throughout, so only the CURSOR ghost was exercised; the queued
   sites go through the same `ghost_one` and the same record, which is an argument and not a
   measurement.
7. **The restorer**, `tagpu_vk_restore.c`. Ported **unchanged**, which needs `spirv-gen.py` to
   emit four variants: `NK ∈ {1, 2, 4, 8}` (`tagpu_restore_glsl.h:10`), `WMAX = NK × kmax`
   (`tagpu_restoreglsl.c:472`). Spec constants cannot do it — `#if NK > 1` declares a different
   number of `out` locations and SPIR-V interface variables are static. Landing 2 makes the lane
   *usable* with Classic++ on; this is what moves the restore itself off GL.

   **SIZED 2026-09-17, BEFORE STARTING, AND THE VARIANT COUNT IS RIGHT FOR A REASON THE ROW DID
   NOT GIVE.** `tools/spirv-gen.py`'s own header refuses to guess at this: *"pre-compiling it means
   enumerating that cross product, which is a decision about what the shipped weights are allowed
   to be… stated in the roadmap rather than decided here."* The cross product is `NK` × `kmax`, and
   `kmax` is read out of the **weights file at runtime** (`load_weights` takes the model by name,
   and `s_w.kmax` is the widest k-block in its layer table) — so a naive reading is that the Vulkan
   lane could only restore with models compiled in.

   **THE CROSS PRODUCT DOES NOT COLLAPSE, AND THE FIRST VERSION OF THIS PARAGRAPH SAID IT DID.**
   [Written 2026-09-17 and corrected the same day, before any code was written.] The argument was
   that `NK` guards extra `out` locations with `#if` and so must be a variant, while `WMAX` appears
   in exactly one place — `layout(std140) uniform WBlock { mat4 w[WMAX]; };` — and is only an upper
   bound, so it could be compiled at the largest shipped `kmax` and every smaller model would still
   be correct. The first half holds. **The second does not**, because it ignores the limit `NK` is
   derived from: `nk = MAX_UNIFORM_BLOCK_SIZE / (kmax × 64)`, clamped to `MAX_DRAW_BUFFERS` and
   rounded down to a power of two — so the block is sized to *fit the device* and inflating `kmax`
   makes it not fit. Concretely, with a 64 KiB limit the `tiny` model picks `NK = 8` at `kmax = 56`,
   `WMAX = 448`, **28 672 bytes**; compiling that same variant at the largest `kmax` would declare
   `WMAX = 8 × 148 = 1184`, **75 776 bytes**, past the limit that chose `NK = 8` in the first place.

   **So a variant is an (`NK`, `kmax`) pair, and both shipped models are real.** Read out of the
   binaries rather than taken from the header comment (`unditherer/models/{tiny,full}.w32.bin`, the
   layer table's widest `kstride`): `tiny` is depth 6 × 24 ch with **kmax 56**, `full` is depth
   12 × 64 with **kmax 148**, and `tagpu_restoreglsl.c:459` selects between exactly those two —
   `load_weights(s_opt.tiny ? "tiny" : "full")`.

   | module | variants | |
   |---|---|---|
   | `FS_VS` (shared by fill and conv) | 1 | |
   | `FILL_FS` | 1 | |
   | `CONV_FS` | **8** | `NK ∈ {1,2,4,8}` × `kmax ∈ {56, 148}`, i.e. `WMAX ∈ {56,112,224,448}` and `{148,296,592,1184}` |
   | `OUT_VS`, `OUT_FS` | 1 each | |

   — **12 SPIR-V modules and 10 program pairings** (fill ×1, conv ×8, out ×1). The reference setup
   uses two of the eight (`tiny`@`NK 8`, `full`@`NK 4`); the rest are for devices with other
   limits, and the `full`@`NK 8` one is only ever selected where `MAX_UNIFORM_BLOCK_SIZE ≥ 75 776`.

   **The decision `spirv-gen.py` asked for, stated:** *the Vulkan lane restores with the two shipped
   models and no others; a new model's `kmax` needs its four variants generated and committed.*
   That is a real constraint on the project and it is the owner's to revisit, not a session's.

   **ONLY ONE STAGE IS VARIANT AT ALL.** Measured over the quoted shader text of each macro in
   `tagpu_restore_glsl.h` rather than over the macro regions — the doc comments mention `NK` and are
   not shader source, which is what makes a naive grep say `FILL_FS` depends on it: `FS_VS`,
   `FILL_FS`, `OUT_VS` and `OUT_FS` reference **neither** `NK` nor `WMAX`. Only `CONV_FS` uses both.

   **What the port is, beyond the shaders**: 1 132 lines of `tagpu_restoreglsl.c`, three programs
   (fill, conv, out) over five shaders, and its render targets are `GL_TEXTURE_2D_ARRAY` layers
   (`glFramebufferTextureLayer`, `glDrawBuffers` with up to `NK` attachments) in `GL_RGBA32F`,
   `GL_RGBA16F` and `GL_RGBA8`. That is the first ported pass whose target is an array texture and
   whose draw is MRT, so neither the seam's render pass nor any sibling's framebuffer shape covers
   it — unlike landing 5, which needed no new mirror, this one needs new *attachments*.

   **THE SHADER HALF IS DONE, 2026-09-17.** `spirv-gen.py` now carries the variant manifest,
   `extract_restore` (the five shaders come out of `tagpu_restore_glsl.h` directly, because they are
   macros pasted at call sites and appear as a `#version` literal in no translation unit) and
   `pp_expand`, a deliberately narrow preprocessor that resolves `#define <ID> <int>` and
   `#if <ID> > <int>` / `#endif` and **refuses everything else** — the tool's own parser has to see
   the final `out` set, so the conditionals cannot survive into the text it reads, and a hand-rolled
   preprocessor that guesses would be worse than none. All eight `CONV_FS` variants reach glslang.

   **Two shapes stop it there, both legal GLSL this tool had never met**, because `CONV_FS` is the
   first shader in the tree to use either:

   1. `uniform highp sampler2DArray uAct;` — a precision qualifier **after** the storage qualifier.
      `_VAR` allows `highp uniform …` and not `uniform highp …`, so the declaration does not match
      at all, passes through unchanged, and glslang refuses it: *"sampler/texture/image requires
      layout(binding=X)"*.
   2. `layout(std140) uniform WBlock { mat4 w[WMAX]; };` — a named block written on **one line**.
      `_BLOCK_OPEN` requires the `{` to end the line, so the block is never seen and never gets its
      set/binding.

   Both were the **tool's to learn rather than the shader's to reformat**: that header is the one copy
   of the text and is shared with `tools/tascene`'s browser pack, and reflowing it for a generator's
   convenience is what its own header forbids.

   **BOTH TAUGHT, AND THE SHADER HALF OF LANDING 7 IS DONE.** `_VAR` captures a precision qualifier
   as its own group, on the other side of the storage qualifier, and the rewriter re-emits each
   group **where it was read** — so `uniform highp sampler2DArray uAct` comes back out spelled the
   same way rather than as `highp uniform …`, which is legal but is not what the shader says. The
   one-line block is handled by `normalise_blocks`, a pre-pass that splits it across three lines on
   the way IN to both the GL side and the translation, so every reader below keeps one code path
   and `residual()` still compares like with like.

   Generated: **46 shaders in 31 programs across 12 headers** — the 34 that were already there plus
   the restorer's 12, paired as 21 + 10, exactly the count this entry predicted. `CONV_FS` at
   `NK 4`, `kmax 148` comes out with `_Globals` at binding 32, `uAct` at 40 *with its `highp`*,
   `uRect` at 41, `WBlock` at 33 holding `mat4 w[592]`, and four `out` locations.

   **And the 34 existing shaders did not move.** `_VAR` and `normalise_blocks` are on every shader's
   parse path, so the whole set was regenerated and diffed: the only change in those eleven headers
   is the `transform` hash line. No SPIR-V word differs, which is the proof the edit touched nothing
   but what it was for. `tools/glslang-fetch.sh` has been run (16.6.0, pinned by version and
   sha256).

   **`kmax` IS READ OUT OF THE WEIGHT BINARIES, NOT WRITTEN DOWN** — `restore_kmax()` opens
   `unditherer/models/{tiny,full}.w32.bin` and reduces the layer table. The first version held it in
   a literal, and the landing's review named the failure that buys: retrain `full.w32.bin` with a
   different widest k-block and neither the shader text nor the tool's hash moves, so `--check`
   stays green while the committed SPIR-V declares a block of the wrong length — found on a device,
   months later. Reading the file makes the weights part of what the headers are generated *from*,
   so changing them fails the build until the headers are regenerated. The binaries are tracked, so
   `--check` can do this in any checkout.

   **AND WHAT IS LEFT IS NOT "PORT A PASS".** Surveyed before writing any of it:
   `tagpu_restoreglsl.c` is not a draw sequence but an **incremental background scheduler** —
   double-buffered GPU **timer queries** with a fallback for drivers that never return one, a
   per-slice **time budget** driven by a smoothed cost-per-unit estimate, **job queues** with
   `pick_job`/`repalette`/`clear` and idle/failed/painted states, and only then the draws (fill,
   `depth ×` conv, out). So the split is not the one the row implies:

   * **API-independent, and it should be shared rather than duplicated**: the queues, the
     scheduler, the budget arithmetic, `tagpu_rglsl_tileable`, the weight-file reader.
   * **What the Vulkan side actually owes**: timestamp queries in place of GL's timer queries, its
     own render passes and framebuffers for `GL_TEXTURE_2D_ARRAY` layers, and MRT attachments per
     `NK`. It is the first ported pass whose target is an array texture and whose draw is
     multi-target, so no sibling's framebuffer shape covers it.

   That is the estimate to plan the next landing against, and it is larger than the row's one line
   suggests — the shaders were the small half.

   **AND THE DEVICE PREREQUISITES ARE ESTABLISHED FIRST, because this plan has twice saved a landing
   by measuring the refusal before porting the stream behind it.** The restorer needs four limits
   and three formats, and every one holds on the reference setup:

   | what the pass needs | it is for | measured |
   |---|---|---|
   | `maxColorAttachments`, `maxFragmentOutputAttachments` | `NK` MRT targets in one conv draw | **8** each — so `NK` up to 8, the same ceiling GL's `MAX_DRAW_BUFFERS` gives |
   | `maxUniformBufferRange` | `WBlock`, `WMAX × 64` bytes | **65 536** — the same 64 KiB GL reports, so the lane picks the same `NK` per model: `tiny` 8, `full` 4 |
   | `maxImageArrayLayers` | one array layer per channel tile, 16 for 64 channels | **2 048** |
   | `timestampComputeAndGraphics` | the slice budget, in place of GL's timer queries | **true** |
   | `R32G32B32A32_SFLOAT`, `R16G16B16A16_SFLOAT`, `R8G8B8A8_UNORM` | the activation, residual and output targets | all three **colour-attachment + sampled + linear-filter** in optimal tiling |

   **The caveat, stated because it is the whole weight of the claim**: those come from `vulkaninfo`
   against the **native** driver, not from the lane through **winevulkan**. Same GPU and same driver,
   and this plan's own coexistence work records winevulkan as a thin passthrough rather than a
   translation — which is why Vulkan was chosen over D3D12 — so they are expected to hold on the
   real path. Expected is not measured: the first thing `tagpu_vk_restore.c` should do is ask the
   device for these itself and refuse with a named reason, the way `lineok` and `flipok` already do.

   **AND THEN THEY WERE MEASURED THROUGH WINEVULKAN, 2026-09-17, BECAUSE "EXPECTED" WAS NOT GOOD
   ENOUGH TO PORT BEHIND.** A 32-bit Windows probe — the same bitness as `ddraw.dll`, run under the
   game's own wine prefix so it loads the same `vulkan-1.dll` through the same loader — asks the
   same questions. Every limit and every format **agrees with the native figures exactly**:
   `maxColorAttachments` and `maxFragmentOutputAttachments` **8**, `maxUniformBufferRange`
   **65 536**, `maxImageArrayLayers` **2 048**, `timestampComputeAndGraphics` **true** at a
   **1.0 ns** period, and all three formats colour-attachment + sampled + linear-filter in optimal
   tiling. The llvmpipe device answers the same, which matters for the headless lane.
   It is still not the same PROCESS as the game, and for the TIMESTAMP prerequisite that residual
   is closed too, from inside the game, with no new instrumentation. `tagpu_vk.c:1599` already asks
   both timestamp questions at device creation — `limits.timestampPeriod > 0` **and** the submitting
   queue family's `timestampValidBits > 0` — and then actually creates a `VK_QUERY_TYPE_TIMESTAMP`
   pool. That block is **unconditional**: the `tagpu_ftime.on` lever gates the *reporting*, not the
   query. And it logs **only on failure**, either *"no timestamp query pool"* or *"this device/queue
   reports no usable timestamps"*.

   **Measured: the lane came up twice in one run (`vk: up in 121 ms`, then `188 ms`) and neither
   failure line appears.** So in the game's own process, through winevulkan, on the family the lane
   submits to, all three hold. The inference is spelled out because it is read off an absence, and
   on this plan reading an absence as a result has already gone wrong once — it is sound here only
   because the code path is unconditional and the lane demonstrably reached it. That is a stronger
   answer than `timestampComputeAndGraphics` on its own: it covers the PER-QUEUE-FAMILY valid bits
   and a real pool creation, neither of which `vulkaninfo` nor the probe checks.

   **And the GL lane's own banner cross-checks the two limits that decide `NK`**, for free, in the
   same log: `restoreglsl: 12x64 fp32, NK=4 (uniform block 37 KB of 64, 8 draw buffers)`. GL reports
   the same 64 KiB block and the same 8 draw buffers Vulkan reports as `maxUniformBufferRange` and
   `maxColorAttachments` — so both APIs agree on the numbers `NK` is derived from, which is what
   makes "the same `NK` per model on both lanes" a measurement rather than an assumption.

   **The probe earned its keep by returning a limit the native run never showed:
   `minUniformBufferOffsetAlignment` = 64** (16 on llvmpipe). A conv draw binds the weight block at
   `(offset + group × kstride) × 16` bytes, and a Vulkan uniform offset must be a multiple of that
   limit. Checked against the shipped weights: **every group offset of both models is 64- and
   256-byte aligned**, so nothing is broken today.

   **But it holds by construction of the exporter rather than by anything the loader checks, and
   the GL lane has the same hole.** `load_weights` validates `kstride & 15` — so
   `group × kstride × 16` is always a multiple of 256 — and validates `offset`'s *bound* but not its
   *alignment*. Offsets come out aligned only because the exporter lays layers back to back from 0,
   making each a running sum of `kout × kstride`. A hand-edited or differently-exported file with
   `offset = 1` would bind at byte 16, which NVIDIA's 64 rejects — and GL's `glBindBufferRange` has
   the identical requirement, so this is a **pre-existing hole in a shipping path, not something the
   Vulkan port introduces**. The fix is one line in the shared loader's existing validation block —
   `(offset & 15)` beside the `kstride & 15` already there, a bound on a value read from a file,
   which is the shape `CLAUDE.md` *Fixes must be safe by construction* asks for. It is **deliberately
   not in the split landing**, whose whole claim is that it changed nothing: adding a validation
   would change behaviour on a malformed file. It goes in with `tagpu_vk_restore.c`, which is what
   makes it concrete.

   **It is not a blocker and nothing stands down for it today** — landing 2 mirrored the restored
   atlases, so the lane draws restored art with the restore itself still running on GL. Landing 7
   is owed to the END STATE rather than to any present refusal, which is why it sits behind 5 and 6
   and ahead of 4.

   **THE SPLIT RAN FIRST, 2026-09-17, AND IT WAS NOT A CHOICE** ([gpu-status](gpu-status.html)
   §2.42). The survey above says the API-independent half *"should be shared rather than
   duplicated"*, and the natural reading of that is a preference between two workable designs. It
   is not: **`tagpu_rglsl_tileable` is called from `tagpu_terr.c:1034` and `tagpu_gaf.c:1130`**,
   both gather halves that survive landing 11, while the same file includes `opengl_utils.h`, which
   landing 11 deletes. So the module could neither go with GL nor stay whole, whatever anyone
   preferred. `tagpu_restore_core.{h,c}` is the scheduler with no API in it;
   `tagpu_restoreglsl.c` is the GL draws behind a twelve-entry backend interface.

   **And it had to run BEFORE the Vulkan half, for two independent reasons.** Extracting after
   writing `tagpu_vk_restore.c` would mean writing a second scheduler and then deleting it — and
   more importantly, **landing 4 takes the oracle away**. A refactor of the GL restorer can be held
   to *"it changed no pixel"* only while the GL restorer is still running: `tagpu_restoredump.on`'s
   terrain atlas came back **byte-for-byte identical across the two builds, 46 461 952 bytes, `cmp`
   clean**, with the four code-determined counts in the `done` line matching (10 036 frames, 4 142
   wrap-padded, 158 batches, 7 426 draws). After landing 4 that claim would be unmeasurable.

   **THE DESTINATION QUESTION IS ALREADY ANSWERED BY GATE 2, and the answer is better than the row
   assumed.** The GL restorer paints into the caller's GL atlas. It was not obvious what the Vulkan
   restorer paints into — but both of a job's surfaces are ALREADY on the device for all four
   consumers: the indexed source is the pass's own atlas image (`s_atImg`) and the destination is
   the restored twin the pass already binds (`s_arImg`, binding 42 in `tagpu_vk_feat.c`, 43 in
   `tagpu_vk_fx.c`). So **the Vulkan restorer needs no new mirror and no read-back at all**; what
   it needs is those images' usage widened to carry `COLOR_ATTACHMENT` and their views lent to it
   as render targets. That also makes the A/B exactly the GL restore against the Vulkan restore
   through an otherwise unchanged pass, and it retires the CPU mirror gate 2 built rather than
   adding a second one beside it.

   **THE FLIP QUESTION IS ASKED AND ANSWERED BEFORE THE DRAWS ARE WRITTEN, and the answer is a
   THIRD case the `flipok` table did not have.** All three of the restorer's fragment shaders index
   the slot grid with `ivec2(gl_FragCoord.xy)`, and GL measures that y from the framebuffer's BOTTOM
   while Vulkan measures it from the TOP — `OriginLowerLeft` is not even permitted in Vulkan. That
   is landing 5b's shape exactly, so it was worked through rather than assumed, and **the two
   differences cancel**: in GL `gl_FragCoord.y ≈ 0` is framebuffer row 0, and for an FBO colour
   attachment framebuffer row 0 *is* texel row 0, with NDC −1 mapping to that same row; in Vulkan
   `gl_FragCoord.y ≈ 0` is framebuffer row 0, which is image row 0, with NDC −1 mapping to it too.
   In both APIs the fragment y is the target's ROW INDEX and NDC −1 is row 0.

   The conventions disagree only about which end of NDC is visually *up*, and that matters only to a
   pass that speaks the SCREEN's y. This one never does: FILL and CONV cover the whole target with a
   full-screen triangle and address it in texels, and OUT places each cell by
   `aPos / uDst * 2 − 1` from atlas coordinates and then recovers the same cell from `gl_FragCoord`.
   So `flipok`'s two rows — *"GL's window convention, must flip"* and *"the engine's y-DOWN screen
   space, must not"* — gain a third: **image space, where the two APIs already agree**, and this
   pass asks for `flipok` at all.

   **AND THE ORACLE FOR THIS PORT IS BYTES RATHER THAN PIXELS**, which makes it the strongest
   instrument on this plan. `tagpu_restoredump.on` writes each restored atlas to disk; the same dump
   taken from the Vulkan lane can be `cmp`-ed against the GL lane's, so *"the Vulkan restore is the
   GL restore"* is a byte comparison of a 46 MB surface rather than a screenshot diff — no window,
   no Route D, no settle heuristic, and a mirrored restore would show as every tile's rows reversed.
   The no-flip conclusion above is recorded rather than trusted: that dump is what settles it.

   **WRITTEN, AND THEN GIVEN A CONSUMER — 2026-09-17** ([gpu-status](gpu-status.html) §2.43).
   `tagpu_vk_restore.c` against that interface: timestamp queries for
   `slice_begin`/`slice_end`/`timer_poll`, the ping-pong activation array images with a view per
   layer, a render pass per `NK` attachment count, the weight UBO bound at a dynamic offset whose
   alignment is a **loader bound** rather than a check at the bind site, and the descriptor sets.
   The device prerequisites are asked and refused by name; the reference setup answers
   *"one k-block 9 KB of a 64 KB block (so NK up to 6), 8 colour attachments, uniform offset
   alignment 64, 16 activation layers of 2048, timestamp budget"* and the lane settles at
   **NK=4, WMAX 592, 37 KB bound per conv draw** — the same NK the GL lane picks for the same
   model, which was the point of cross-checking the two limits against GL's banner above.

   **The consumer is the terrain atlas, and the destination question answered itself exactly as
   this entry predicted**: `COLOR_ATTACHMENT` added to the restored twin's usage and its view lent
   per job, no new mirror and no read-back. What crosses the hand-over is the **frame list** — the
   gather half keeps building it (`wrap` and the centre-out order are engine-memory facts) and the
   `_vk` half draws it, which is the gather/pass line doing exactly what it was drawn for.

   **AND THE BYTE ORACLE PAID FOR ITSELF ON ITS FIRST RUN.** Both lanes restore in the same
   process on the same frames, so the two dumps are one `cmp` apart. Every code-determined count
   matched — 10 036 frames, 4 142 wrap-padded, 158 batches, 7 426 draws, both lanes — and
   **6 936 texels of 11 615 488 differed, which is exactly 6 × 34², the cell pitch squared**: six
   whole cells of 10 036 entirely unpainted, every other cell byte-identical. Cause:
   `tagpu_rcore_step` can issue **more than one batch in a slice** (the loop re-picks at every
   batch boundary and runs until the time budget is spent — batches 157 and 158 both issued at
   slice 917), and the OUT draw staged its vertices per **frame slot**, so the second batch
   overwrote the first's vertices before either draw ran. A screenshot diff would have called this
   clean; six transparent cells in a 46 MB atlas is what the instrument was chosen for.

   The fix is an **ordering** and not a bigger arena — batches per slice is a time budget rather
   than a count, so any arena is a number that can be exceeded and what it buys is this failure
   again. `vkCmdUpdateBuffer` puts each batch's vertices in the command stream at the point of its
   own draw. Wiring the consumer also found the restorer's render passes declaring **no subpass
   dependencies at all** and `dst_ready` discarding a repaint's destination; both are in §2.43.

   **AND TWO MORE CONSUMERS, WHICH FOUND THE SAME BUG IN ITS SIBLING — 2026-09-17**
   ([gpu-status](gpu-status.html) §2.44). Features and effects are wired, and what crosses is a
   different shape from terrain's because the queue is: a GAF atlas is a **lazy queue**
   (`tagpu_gaf.c`'s `restore_enqueue` adds one frame per miss for the life of the atlas), so
   the hand-over is an **append-only list with a generation** and the consumer holds a **cursor**
   into it. A frame on which the consumer took nothing costs nothing; a recycle, a repack, a
   context loss or a palette move arrives as a new generation and rebuilds the job from index 0.
   The list is bounded at four times the atlas's entry ceiling, and reaching it restarts from the
   entries the atlas actually holds — the same function the arm uses, so the rare path is the one
   exercised on every session's first frame.

   **The bug was the parameter tables, staged per FRAME SLOT — landing 7c's own finding one
   resource over.** `upload_tables` wrote each batch's destination rect, source rect and **key and
   wrap** into a host-mapped region indexed by the slot, on a comment saying one batch is ever in
   flight: true of the device, false of the recording. Both batches of a two-batch slice wrote the
   same address before either copy executed, so the first batch's frames were restored through the
   second batch's tables. **118 of 1 304 feature frames and 3 of 167 effects frames**, with 13 803
   texels holding the key's own palette colour opaque where the GL twin writes `(0,0,0,0)` — a
   frame restored through another frame's key has no keyed texel where it should have one. The
   terrain could not have shown it: one tile size, no colour key, so a swapped table costs a source
   rect and nothing else. Fixed the way 7c fixed the vertices, re-measured with the collision
   exercised (62 two-batch slices), and all three atlases are byte-identical on two fixtures.

   **AND THE TERRAIN NEVER HIT THIS BUG AT ALL, which the landing review established and the first
   version of the write-up got wrong.** `batch N … issued at slice M` is logged at the batch's
   **OUT**, so two of those lines sharing a slice is 7c's vertex condition; the tables ride the
   **FILL**, and the budget check sits between individual draws, so a 64-frame batch's FILL is
   slices earlier than its OUT. The sprite atlases' batches are small enough for two whole
   FILL→OUT chains to fit in one slice, which is why they found it on the first run.

   **What is not done**: the UNITS, which are the fourth consumer and carry a seam of their own —
   their twin is mipped and trilinear, and a Vulkan restore paints level 0 only.
   The UI atlas is a fifth consumer and keeps its read-back. `repaint` is still unexercised.

   **AND THAT SEAM IS MEASURED RATHER THAN ARGUED — 2026-09-17** ([gpu-status](gpu-status.html)
   §2.45). This entry said the landing "has to answer where the levels come from without guessing
   at `glGenerateMipmap`'s reduction", and `tagpu_gaf.h` has said the same since gate 3a. It is a
   question this repository can ASK: the dump now writes the whole chain and each level was held
   against six candidate reductions. The driver performs an **unweighted 2×2 box average of
   RGBA** — alpha-weighting and gamma-awareness are ruled out by a maximum error of 57 and 54
   levels on one channel (while both still match 97 % of texels, which is why the maximum is what
   was reported), alpha comes back **exact**, every integer rounding lands **within ±1 per RGB
   channel**, and the residual sits on fully opaque quads rather than on mixed-alpha edges, which
   is a rounding rule and not a different filter.

   So the landing has three options and a recommendation instead of an open question: keep the
   read-back (byte-identical, and it would be the last one on this plan), reduce on the Vulkan side
   (within 1 LSB — a measured bound rather than equality), or **reduce on BOTH lanes with our own
   pass**, which is byte-identical by construction on every driver for ≤1/255 on the shipped GL
   twin's minified texels. The third is the recommendation; the measurement is what makes it a
   small decision rather than a leap.

   **7e-1 TOOK OPTION C, AND ITS GL HALF IS LANDED — 2026-09-17** ([gpu-status](gpu-status.html)
   §2.46). `TAGPU_RESTORE_MIP_FS` is the exact integer 2×2 box average and `tagpu_rglsl_mips` draws
   it per level; `twin_mips` calls it and keeps `glGenerateMipmap` as the fallback. **The chain is
   now 100.00 % `(sum + 1) / 4` of the level above at both levels — 1 048 576 and 262 144 texels,
   max |Δ| 0 on every channel including alpha** — so the GL lane's levels are a formula rather than
   a driver's rounding rule, and the formula lives in one shader string both lanes compile. The
   three sprite pairs stayed `IDENTICAL` on the same run. A defect found by reading the change
   rather than by measuring it: the twin allocated **level 0 only** and every other level existed
   because `glGenerateMipmap` created it, so a reduction drawing into level 1 would have found an
   incomplete framebuffer, fallen back silently, and a twin painted once would have kept the
   driver's chain for good — every level is allocated at creation now.

   **7e-2 IS LANDED — 2026-09-17** ([gpu-status](gpu-status.html) §2.47). The unit consumer paints
   its own twin on the Vulkan lane: level 0 through the OUT pass, levels 1..mip through the same
   integer `(sum + 1) / 4`, per-level views on the twin, and the oracle extended from level 0 to a
   `cmp` of **whole chains**. **`unit CHAIN: IDENTICAL, 22 020 096 bytes` — level 0 and both mip
   levels, three consecutive runs, with `terr`, `feat` and `fx` unchanged at IDENTICAL.** That is
   all four consumers of the restorer agreeing byte for byte on the Vulkan lane.

   Most of the landing's length went on one defect, and its shape is the part worth carrying
   forward. The first one or two unit batches painted **black** and every later batch was exact.
   The cause was outside the restorer: `tagpu_vk_unit_upload`'s **feed** path — the frame that exists only to
   make the job — left through a stand-down that frees the slot, and that frees the staging buffer
   a `vkCmdCopyBufferToImage` recorded moments earlier still reads. The lost copy is not the
   damage; the damage is that `atlas_upload` latches `s_atHave`/`s_atSerial`/`s_atRows` at **record**
   time, so the pass then believed the device held rows it had never received and handed the
   restorer frames `covered_prefix` called covered. The lane restored from an empty image, and
   `frag = c - net` with both terms at palette index 0 is black. Fixed as a lifetime: the feed path
   destroys nothing, exactly as `refuse` already did not.

   **Two things this cost that the next landing should not pay again.** The oracle's
   `unit SOURCE: IDENTICAL` line compares both lanes' sources *after everything has settled*, so it
   cannot see a source that was empty during the restore — it asserted "the same bytes, restored
   differently" and the premise was false. And the thing that finally identified it was making the
   OUT shader **report the index it had read** on both lanes; reading the device image back early
   then showed 404 798 texels missing where the same read-back taken later is exact. When a
   dependent lane's picture is wrong, measure its INPUT at the moment of use before reasoning about
   its arithmetic.

   **The review returned four code findings and one doc error, all verified against the source and
   all acted on** — and three of the four are about paths the reference setup's driver never takes,
   which is the argument for a reader rather than another run: a level-0-only twin (reachable
   whenever `glGenerateMipmap` does not resolve) drew **no units at all for the session** because
   the OUT pass paints through a level-0 view that was only created for `mips >= 1` and `job_new`
   refuses a null view silently; the "twin moved under a live restore" guard could not fire in two
   of the cases it exists for and ran later in the frame than the destruction it guarded; the
   forced first mip reduction sampled level 0 while it was still `UNDEFINED`, on the ordinary first
   frame; and a degenerate list frame turned into a permanent coverage boundary. The chain
   re-measured `IDENTICAL` on both fixtures afterwards.

   **Still open after 7e-2:** the same record-time latch exists in `tagpu_vk_feat.c` and
   `tagpu_vk_fx.c` and is unreachable there rather than absent — neither has a stand-down below its
   staging allocation today, and one added would re-open it. `tagpu_vk_restore_job_repalette` still
   has no caller while the GL side's does, so a palette that moves in play desynchronises the two
   lanes. [RE-CHECKED 2026-09-18 and both halves hold: `tagpu_vk_restore_job_repalette` and
   `tagpu_vk_restore_job_clear` have a definition and a declaration and **no callers at all**,
   while the GL counterparts are called at `tagpu_gaf.c:1050` and `:272`. So on `renderer=vulkan`
   neither the atlas clear nor the palette follow happens.] `repaint`, the blank counter, the cleared-picture flag, the out-of-memory list drop and
   the dump's retire remain unexercised, and `job_clear` has no callers.
8. **`PK_PIXELS` closed.** `tagpu_gui_hook.c:330`'s op kinds `OP_LINE`, `OP_BAR`, `OP_RECT`,
   `OP_FRAME` and `OP_SCALE` publish through `pub_surface_bytes` at `:1489` — *the engine's
   surface bytes as they stand at the flip*. They become drawn geometry with their own packet
   kinds. Oracle: `strict`.

   **This is LANDING 8a OF FOUR, and the split is the op kinds' own** — each is a different engine
   function with a different shape, and one packet kind per landing is the smallest thing that can
   be run and measured. **8a LANDED 2026-09-18** ([gpu-status](gpu-status.html) §2.58): `OP_BAR`,
   the engine's `DrawBar 0x4BF6F0`, becomes `PK_BAR` — a box and one palette index, nothing in the
   arena — replayed by `vkCmdClearAttachments` on `TAGPU_GUIOP_CLEAR`'s path, so it takes no draw
   slot, no quad and no descriptor set. Five parts, because the consumer has two layers: the
   observer's colour capture, the packet kind, the GL twin's `twin_fill`, the mirror op, and the
   Vulkan draw. Measured on `renderer=vulkan` with the full play arm set at 1024x768: **`bar 1334`
   observed, `bars=158` replayed, the frame at 0 magenta of 786 432** with the health bars under
   each unit.

   **The colour field's width was established before the packet carried one** and landed separately
   (`f8c1b6b`): `DrawBar`'s writer `0x4CCDEA` takes the low byte of its colour and nothing wider,
   so `PK_BAR::fg` is one `unsigned char` by the engine's own width.

   **That landing also claimed the same of three sibling functions and was wrong about all three**
   — corrected here by 8a's review. `0x4BF8C0` writes through `0x4CC7AB` (×10), `0x4BF7B0` through
   `0x4BEC70` (×8), and **`0x4BF4D0` does not fill at all**: it clips once and then remaps every
   pixel already in the box through one of 32 256-byte rows at `globals+0xC4` (darken) or `+0xC8`
   (lighten), chosen by a *signed level* clamped to `[-0x20, +0x1F]`. So `0x4AA912`'s `-0x18` is
   darken level 24, not palette index 232, and the survey that guessed "shade mode" was right. The
   shipped path is unaffected — `publish()` reads the colour for `OP_BAR` alone — but the plan for
   8d changes shape below.

   **8a is 0.02 % of the traffic and the plan should say so.** The live census is
   **`rect 5 396 343`, `line 3 614 453`, `bar 1 334`**. `OP_BAR` went first because it is the only
   one of the five with an unambiguous shape — one engine function, one solid fill. The rest, in
   the order their unknowns have to be answered:

   * **8b — `OP_RECT`.** The volume, and it conflates two engine functions:
     `DrawTranspRectangle 0x4BF8C0` is **hollow**, four edges through the store-only Bresenham
     `0x4CC7AB` and not one fill through `0x4CCDEA`, and the focus rectangle `0x4BF7B0` is a third
     shape again. Disentangling those three is 8b's first act.
   * **8c — `OP_LINE`. LANDED 2026-09-18** ([gpu-status](gpu-status.html) §2.60). The largest
     portable kind. It did **not** need a direction bit: it needed the axis-aligned/diagonal
     **split**, decided from the endpoints before the box exists. An axis-aligned line's bounding
     box **is** the line, one pixel thick, so it publishes as **`PK_BAR`** — same packet, same
     twin fill, same clear, and **zero new enumeration sites** for either consumer. A diagonal's
     box is the square the line crosses, which is exactly [gui-renderer](gui-renderer.html) §20's
     cyan squares, so `OP_DIAG` keeps `PK_PIXELS`.
     **`diag` measured 0 in every session taken** (`line 800 315 / diag 0` in play), **but that
     is a result and not a property** — 8c's review corrected the claim. `markown.on` suppresses
     the engine's box **per unit** and only while `tagpu_native_selbox_complete()`, which drops
     whenever the native pass comes up short; a measured frame (`tagpu_native.c:3795-3803`) had it
     hand ~460 boxes back. Forcing a diagonal deliberately needs `mark.on=noselbox`, a unit
     selected, and an **orientation** off the axis — not merely a heading, since the engine uses
     all three angles — which gives `line 1 548 252 / diag 111 603`. So 8c closes essentially all
     of `line` as measured, and `OP_DIAG`'s safety rests on its `PK_PIXELS` fallback rather than
     on diagonals being absent.
   * **8d — `OP_FRAME` (`0x4BF4D0`) and `OP_SCALE`, and 8d is now the ODD ONE OUT.** `0x4BF4D0` is
     a **shade of the destination**, not a fill: there is no colour to publish, only a level and a
     dependence on what is already in the box. Every other kind in landing 8 replaces *published
     bytes* with *a description of a draw*; this one's draw READS the surface it writes, so the
     Vulkan lane would have to sample its own twin and write it back — a different mechanism from
     8a/8b/8c, not a fourth instance of the same one. **`OP_SCALE` IS NOT A SCALED BLIT AND CANNOT GO
     WITH `PK_SPRITE`** — this entry said it could until 2026-09-18, and the engine map already
     disagreed. `before_scale` hooks **`0x4C7580 GAF_DrawTransformed(ctx, src, int xy[6],
     int uv[6])`**, a **textured TRIANGLE**: three screen vertices and three texture coordinates
     (measured `(214,94)(233,94)(233,113)` with uv `(1,1)(31,1)(31,31)`; it draws the option
     screens' backdrop, *"which is why that region has slanted edges"*). `PK_SPRITE`'s contract is
     *a plain keyed GAF blit* — an axis-aligned QUAD with a resolved atlas rect — and a triangle
     with arbitrary UVs is not one. The op's recorded box is the vertices' bounding box, and a
     triangle covers about half of it. **Not a bug today** (it publishes `PK_PIXELS`, whose bytes
     are exact), but unportable by every mechanism landing 8 has: the clear path draws
     axis-aligned constants and the sprite path draws quads. It needs a vertex buffer and a
     pipeline the UI lane does not have. **6 967 ops** in the measured game. **Whether 8d is one landing, two, or
     a decision to leave `OP_FRAME` as `PK_PIXELS` is open**, and that last option is a real one:
     a tint of the destination is the one case where publishing the destination's bytes is not
     obviously the wrong answer. [The framed box's nature was established by landing 8a's review;
     before it this entry said "three fills", which is what the engine map had said since G15a.]

   **AND THE PATTERN BEHIND ALL THREE CORRECTIONS, SO IT IS NOT REDISCOVERED A FOURTH TIME.**
   `0x4BF4D0` was filed as "three fills" and is a shade; `OP_RECT` was filed as "the volume" and is
   99.7 % a tint; `OP_SCALE` was filed as "a scaled blit" and is a textured triangle. **The op
   kinds were named from the OBSERVER's point of view, and the observer sees a box** — every leaf
   is recorded as a rectangle, because a rectangle is what the census needed. The name therefore
   describes the RECORD and not the engine function, and the only cure is to read the function
   before planning around its name. 8b's `focus`/`rect` split and 8c's `line`/`diag` split are the
   same move twice: give the thing its own kind so the census counts it and the name stops lying.

   **THE GATE'S SHAPE AFTER 8a AND 8b, WHICH IS NOT THE SHAPE IT WAS FILED IN.** Two landings in,
   the ops split into two classes rather than five kinds, and only one class is what landing 8
   assumed:

   * **Replaceable by a description of a draw** — `bar`, `rect` and `line`, **all three now done**.
     These write a constant and read nothing. What is left of this class is `OP_DIAG` (needs a
     line rasteriser in the twin, and 0 ops under the shipped arm set) and `OP_SCALE` (a scaled
     blit, which belongs with `PK_SPRITE`).
   * **Destination-dependent tints** — `focus` (`0x4BF7B0`, **1 223 310 ops**) and `frame`
     (`0x4BF4D0`). Both read the pixels they overwrite and remap them through a LUT. **A colour
     and a box cannot express either**, and on the Vulkan lane a consumer would need to sample the
     twin the pass is writing — a subpass input or a second pass, not a clear.

   **So the single largest consumer of `PK_PIXELS` among these leaves is the class the gate has no
   mechanism for**, and it was invisible while `focus` and `rect` shared an op kind. **Whether the
   tints port at all is now an open question rather than a queued task**, and leaving them as
   `PK_PIXELS` is defensible for precisely the reason they are hard: publishing the destination's
   bytes is what a destination-dependent op means. **That is a decision about the gate's exit
   condition, and it is the owner's** — the exit condition as written ("`PK_PIXELS` closed") cannot
   be met by 8c alone, and nothing here rewrites it.

   **Not covered by 8a and 8b:** `PK_PIXELS` is not closed — `focus`, `line`, `scale` and `frame`
   still publish surface bytes at the flip.
9. **Seeds carry art.** — **LANDED 2026-09-18**, and read the three corrections below before
   quoting anything else in this entry. A `PK_SEED` is published lazily on first touch
   (`tagpu_gui_hook.c`'s `pub_seed`) because we cannot know how a surface got its contents.
   The fix is to make the engine redraw: `gui-renderer.md:55` has the panel as a pre-rendered
   surface at `panel+0xBC` whose gadget handlers sit behind the type dispatcher **`0x4A9176`**
   with table **`0x4A962C`**. `GUI_StageUpdateDraw 0x4A81E0(gi, 0x40)` is issued at the flip's
   **return**, on the game thread, with `s_inFlip` already cleared (the leaves drop every op
   inside the flip), refused unless `TheActive_GUIMEM`, its `ControlsAry` and `panel+0xBC` are
   all present, and non-reentrant by a flag. Off with the `norepaint` token.
   **Measured: the GUI atlas holds 28 frames against `norepaint`'s 19**, three boots each,
   `renderer=vulkan`. Full write-up: `gpu-status.md` §2.61.

   **CORRECTION 1 — "every pixel arrives as an op" IS FALSE, and the disassembly said so before
   the measurement did.** With `0x40` the path always reaches `0x4A90F4`, which repaints the
   **whole panel surface from a bitmap** before a single gadget is drawn —
   `0x4C6B70(panel+0xBC, GUIMEM+0x24, 0, 0)` when that field is set, else the picture handler
   `0x4B0230(gi, 0, panel+0xC4)`. The gadget **chrome** comes back as draws; the **wallpaper**
   comes back as a copy, and that copy is only a win while its source is a surface we twin.

   **CORRECTION 2 — the trigger is `g_guiq.resets`, not a level change.** The packet's level
   generation advances in `tagpu_packet_pub_level_end`, i.e. when a level is **torn down**, so
   shadowing it fired exactly once per session — at the arm — and never on entering a game. It
   did not need to: entering a game *builds* the in-game screen, and a build already draws every
   gadget through the leaves. What actually causes a seed is a **reseed** clearing `seeded` on
   every surface at once, and a level boundary is one of its four causes (with the consumer
   stalling over, a lost sprite and an arena overflow).

   **CORRECTION 3 — this is a SHELL mechanism.** One redraw of `MAINMENU.GUI` produces **115
   ops** (`gaf 105  line 4  focus 6`). One of the in-game `ARMMAIN2.GUI` produces **1**, because
   that screen is three labels (`KILLS`, `LOSSES`, `TOTALUNITS`) — the in-game panel art is drawn
   by the in-game draw path, not by this gadget tree.

   **CORRECTION 4, FROM THE LANDING REVIEW — A REDRAW WRITES MORE THAN PIXELS.** `0x4A943B`'s
   focus branch calls `0x4A16F0`, which sets the GUI dirty flag `gi+0xCCA` and stamps gadget
   `+0x1F`; the pump `0x4A9FD0` clears that flag at `0x4AA0AF` and answers it with **another**
   `0x4A81E0(gi, flags | 0x40)`. **Whether that amplifies is NOT settled**, and this entry said
   it was until the sample was taken properly: `builds=` over three boots per arm gave overlapping
   means with no direction (1.0/72.0/69.1 shipped, 28.1/21.8/62.1 under `norepaint`). `buildFlags`
   is 0xC0 in every window of both arms — the engine redraws continuously by itself — and no
   runaway was observed, which is the weaker thing the evidence supports. The review also corrected
   "the two frees" to **six** (the raw `0x4D85A0` at `0x4A9575`/`0x4A95A7` as well), all inside
   the same `test bl,0x2` gate, so the conclusion stands on the gate rather than on a count.

   **NOT CLOSED BY 9:** `PK_SEED` itself. A surface is still seeded on first touch after every
   reseed; the repaint replays *over* that seed rather than instead of it. What changes is that
   the art also exists as sprite ops, so the atlas holds it — which is what G15e's *"seeded art
   stays indexed until repainted"* actually needed.

   **BOTH OF THIS ENTRY'S OPEN QUESTIONS ARE ANSWERED [DISASSEMBLED 2026-09-18], and the second
   answer is NO.**

   *The entry point* is `GUI_StageUpdateDraw 0x4A81E0` itself — the dispatcher sits inside its
   draw loop. The loop reads the gadget count from `panel+0xB6` (a **signed WORD**; `jle` at
   `0x4A9148` means a count ≤ 0 draws nothing), walks records from `panel+0x15B` at a stride of
   **`0x15B`**, skips any whose byte at **record+0x29** is zero, and dispatches on the type byte
   at **record+0x00** (`0x4A9176`: `cmp eax,0xC / ja / jmp [eax*4+0x4A962C]`). `0x40` is the
   redraw bit, tested per gadget type at seven sites inside the loop (`0x4A92C5`, `0x4A92E7`,
   `0x4A932B`, `0x4A934D`, `0x4A9398`, `0x4A93D8`, `0x4A93F3`).

   *Is it safe to call twice?* **YES for a redraw-only call — and the first version of this entry
   said the opposite.** [CORRECTED 2026-09-18, same day.] It claimed *"a second call LEAKS BOTH
   PANEL SURFACES"* because *"the draw half allocates unconditionally"* and *"the sole early return
   is `gi->[0x18] == NULL`, so every real call falls into the allocation"*. **The allocations are
   gated on the BUILD flag.**

   `0x4A82F7` computes `eax = ebx & 1` — the `0x1` **build** bit — stores it at `[esp+0x18]`, and
   `je 0x4A90D1` when it is clear. `0x4A90D1` is **past both allocations** (`0x4A907C` and
   `0x4A90B5`), and from there `test bl,0x40` at `0x4A90DE`/`0x4A90EF` carries a redraw on into
   the gadget loop at `0x4A9135`. So:

   * **`0x1` build** — allocates `panel+0xBC` and `panel+0xB8` through `0x4C69F0`, which is an
     allocator and not a find-or-create (`0x4D83B0(name, w*h + 0x30)`, and `0x4D83B0` **drops the
     name**, reading only `[esp+0x8]`). Calling *build* twice without a teardown is what would
     leak.
   * **`0x40` redraw** — **allocates nothing**, frees nothing, and reaches every gadget. This is
     the call landing 9 wants.
   * **`0x2` teardown** — the only frees, `0x4A9537` and `0x4A9549` → `0x4C6AC0`, then both
     pointers NULLed.

   Mechanically confirmed over the whole function: **exactly two** calls to the allocator and
   **exactly two** to the free, at those four addresses and nowhere else.

   **So landing 9's shape is the simple one after all: `GUI_StageUpdateDraw(gi, 0x40)` after arm
   and after a level-change reset.** No free of ours, no rebuild, no new invariant — options (a)
   "free both ourselves first" and (b) "teardown + rebuild", which the first version of this entry
   listed as the only ways, are both unnecessary and (a) was actively dangerous.

   **Two other paths skip the allocation and are NOT the redraw path**, found by the same
   re-check: `0x4A8349` and `0x4A835A` jump to `0x4A95E3` when `0x4B6700()`/`0x4B6710()` — the
   screen width and height — are smaller than the panel's own `+0x17`/`+0x19`. A panel bigger than
   the screen abandons the whole call.

   **HOW THE FIRST VERSION WENT WRONG, because it is the same error three other entries on this
   gate carry.** It looked for early **returns** before the allocation and found one, and did not
   look for **jumps past** it, of which there are three. A mechanical check that enumerates one
   kind of control transfer and not the others is the same failure as counting `call` sites without
   following the control flow, and as grepping a shader over a truncated range. The check that
   works: enumerate **every** branch and return before the point of interest and ask where each one
   lands.

   **MEASURED 2026-09-18, and this paragraph used to say NOT MEASURED:** a repaint does produce
   ops, and `record+0x29` is not a dirty bit — it is the gadget's own `active` flag
   (`gui-gadgets.md` §1.1), so a forced redraw re-issues the draws for every gadget the engine
   would have drawn itself and for no others. 115 ops on `MAINMENU.GUI`, 1 on the in-game
   `ARMMAIN2.GUI`; see CORRECTION 3 above for why those two numbers are not in tension.
   **Rejected:** submitting the seeded surface to the restorer as a job. Its contract
   (`tagpu_restoreglsl.h`) is *a frame of art from an R8 atlas with its colour key*; a seeded
   panel is a composite of art, glyphs and chrome with no key and no tileability, the model was
   not trained for it, and `uidiff` has no cell to match it against.
10. **The engine-frame fallback layer goes** — **LANDED 2026-09-18, AND NOT AS THIS ROW READS.**
   Neither half of the title survived contact with the code: the *layer* does not go (in the
   Vulkan lane `tagpu_vk_surf.c` draws TA's frame opaque as the bottom layer and nothing else
   would), and `uSurf` does not go (the stale-mirror guard reads it). What went is a **duplicate
   upload left behind by 4c** — this plan's own 4c section called the resolve *"largely an
   ownership move"* and named `s_engImg` as machinery *"in the wrong owner"* (:501); 4c moved the
   producer and left the second consumer standing, so landing 10 finishes that move rather than
   discovering an oversight. The lane was copying TA's frame into *two* R8 images every frame,
   `tagpu_vk_surf.c`'s for the bottom layer and `tagpu_vk_gui.c`'s `s_engImg` for `uSurf`, from
   the same `tagpu_surf_frame` source — 786 432 bytes at 1024×768, twice. The UI pass now borrows
   the surface pass's image through `tagpu_vk_surf_engine_view`, on an ordering the seam already
   had. Full write-up and the measurements: `gpu-status.md` §2.62.

   **The comment that justified the second copy reasoned from the GL lane**, which went in
   4d-1/4d-2 — a stale justification outliving the thing it named, which is the same failure as
   landing 11's headline being a comment.

   The original text follows. Its own correction below it is right about `uSurf` having two
   consumers and wrong in its last two sentences, which are struck in place rather than left to
   be read as current:

   `tagpu_gui_surf.c:51` has it as
   the bottom of three; G15b, G15c and G17d measured **0 holes** across 120 stops, so nothing
   reads it.

   **THE PREMISE IS FALSE AS STATED, AND THE FIRST VERSION OF THIS PARAGRAPH — COMMITTED IN
   `f5e4d33` — WAS WRONG.** It claimed that "over the whole of `LAY_FS` (`tagpu_gui_surf.c:465-590`)
   the sampler `uSurf` occurs twice: the declaration and a comment. There is no `texelFetch` and no
   `texture()` against it", and concluded that the landing was a saving. **`LAY_FS` runs to line
   642, not 590**; the range was computed by a heuristic that stopped at the first line ending in
   a semicolon and truncated the shader. Over its real extent `uSurf` occurs **five** times, and
   **two of them are `texelFetch`**.

   **The engine frame has TWO consumers in `LAY_FS`, and one of them ships:**

   * **The stale-mirror guard** (`:630-635`, under `uGuard`, which the hand-over sets at
     `:2670` from `have_engine_frame` — `f->surface_tex != 0 || tagpu_vk_owns_present()` at `:809`,
     so it is ON in the shipped Vulkan build; `:2606`, which an earlier version of this line cited,
     is the GL lane's uniform and is dead here). Where the twin reads index 0 and
     the engine's surface reads something else, the fragment is **discarded**. This is what stops
     the layer painting stale black over the intro Smacker, which writes the primary directly so
     no op ever reaches the queue ([gui-renderer](gui-renderer.html) §21.2). **Deleting `uSurf`
     would re-open that.**
   * **The `uStrict` harness** (`:638-641`), the A/B's magenta marker. Armed only by
     `gui.on=strict`.

   **So what is true is narrower than the row claims.** The engine frame is not composited as a
   bottom LAYER — where the twin has no coverage the shader `discard`s rather than blending it —
   and that is what the 120 stops measured. But it is **read**, as a reference, by a guard the
   shipped build depends on. **The row's "nothing reads it" is about the layer role only**, and
   what this
   paragraph concluded from that is **STRUCK — the landing disproved it** [2026-09-18, found by
   landing 10's own review, which caught the paragraph above blessing this one wholesale]:

   > *"landing 10 is therefore not a deletion of `uSurf` but a deletion of the layer semantics,
   > with the guard's read kept. That is a different and much smaller landing, and the 786 432
   > bytes a frame the Vulkan lane uploads are not waste: the guard needs them."*

   Landing 10 deleted **neither** `uSurf` **nor** the layer semantics — it changed who owns the
   image behind them. And the 786 432 bytes a frame **were** waste, though not the ones this
   paragraph was looking at: the guard does need the first copy, and the lane was uploading a
   **second** one from the same source, which this paragraph did not know existed. Everything
   before the quote still stands: `uSurf` has two consumers and deleting it would re-open the
   Smacker case.

   **The trap that produced the wrong version is worth more than the finding.** A range computed
   over a multi-line C string constant, then grepped, gives a confident answer about text it never
   read. It is the same failure as counting `call` sites without following the control flow
   (`0x4BF4D0`, `0x4BF7B0`): **a mechanical count over a boundary nobody checked.** Find the
   string's real end before trusting a count inside it.

   **And `uEng` is NOT `uSurf`**: `MM_FS`'s `uEng` is the MINIMAP's engine picture
   (`s_mmEngView`), sampled at `tagpu_gui_surf.c:382` and `:394` — the reason there is no
   radar-arc replay. Nor is this 4c-1's bottom layer (§2.52). Three different things called "the
   engine's frame", which is its own reason this entry went wrong.
11. **The deletion landing** — `render_ogl.c`, `render_d3d9.c`, `opengl_utils.c`,
   `openglshader.h`, `render_ogl.h`, and **`tagpu_restoreglsl.c`**. `renderer=gdi` becomes the
   documented stock reference.

   **BLOCKED ON LANDINGS 10b AND 10c** (*What the seam has to grow* below): the structure-shadow
   gate, and the fact that **no `tacli` verb works on the gdi lane at all** — the on-demand
   trigger family is called only from `tagpu_overlay_draw`, which only the GL and Vulkan backends
   call. Until 10c, this item's exit condition ("`renderer=gdi` becomes the documented stock
   reference") cannot be checked by anything we have.
   `renderer=gdi` is not stock today, and the survey narrowed that from a class of suppressors to
   exactly two bytes: the `je`->`jmp` pair at `0x4592C6`/`0x45952C`, which has no runtime gate to
   be inert through, so the gdi lane draws no structure shadows and nothing of ours draws them
   either. This item's headline claim is false until that is fixed.

   **AND THE HEADERS, WHICH THIS ITEM LISTED AS "UNCHECKED", CHANGE ITS SIZE** [surveyed
   2026-09-18; the survey's own first pass was wrong, see the method note below]. The six files
   this item names are ~5 200 lines. They are not the job.

   **`opengl_utils.h` is the whole GL entry-point surface** — 94 declared symbols, every `gl*`
   the tree calls, `glcorearb.h` behind it — and **twenty `.c` files outside the deletion set
   include it and use it**:

   | file | lines carrying a GL symbol | uses |
   |---|---|---|
   | `tagpu_gui_surf.c` | 247 | 268 |
   | `tagpu_native.c` | 215 | 241 |
   | `tagpu_posedraw.c` | 110 | 121 |
   | `tagpu_terr.c` | 107 | 121 |
   | `tagpu_hires_draw.c` | 82 | 87 |
   | `tagpu_mark.c` | 71 | 82 |
   | `tagpu_fx.c` | 63 | 77 |
   | `tagpu_shadow.c` | 54 | 61 |
   | `tagpu_gaf.c` | 53 | 58 |
   | `tagpu_feat.c` | 45 | 57 |
   | `tagpu_scaffold.c` | 40 | 48 |
   | `tagpu_posebake.c` | 31 | 31 |
   | `tagpu_fps.c` | 30 | 38 |
   | `tagpu_hires.c` | 30 | 31 |
   | `tagpu_overlay.c` | 21 | 25 |
   | `tagpu_text.c` | 20 | 20 |
   | `tagpu_render3do.c` | 10 | 10 |
   | `dd.c` | 2 | 2 |
   | `tagpu_ftime.c` | 1 | 2 |
   | `render_gdi.c` | 1 | 2 |
   | **total** | **1 233** | **1 382** |

   **So deleting `opengl_utils.c` and its header means deleting the GL half of seventeen passes**,
   not resolving one caller of `oglu_load_dll`. Those halves are already inert under
   `renderer=vulkan` — each sits behind its pass's own `gl_draws = !tagpu_vk_owns_present()` — so
   this is deletion rather than porting, and the lane's behaviour does not change. But it is
   **1 233 lines across seventeen files that each need their own before/after**, and it dwarfs the
   ~5 200 lines of the six files the row names. The three one-line users are trivial by
   comparison: `dd.c`'s `oglu_load_dll`, `render_gdi.c`'s `g_oglu_version`, `tagpu_ftime.c`'s
   `xwglGetProcAddress`.

   **AND NOT ONE OF THEM IS A FILE TO DELETE.** [The first version of this paragraph said
   `tagpu_shadow.c` and `tagpu_hires_draw.c` *"go whole with the lane"*. **Both are wrong**, and
   the check that found it is the one this entry keeps having to relearn: ask who CALLS it, not
   where its guard sits.] Every non-static function each pass defines was matched against every
   `tagpu_vk*.c` and `render_vk.c`, and **sixteen of the seventeen are called by the Vulkan
   lane**:

   | pass | what the Vulkan lane calls |
   |---|---|
   | `tagpu_gui_surf.c` | `tagpu_gui_handover`, `tagpu_gui_mirror_want`, `tagpu_gui_mirror_reseed`, `tagpu_gui_cursor_drew_take`, `tagpu_classicpp_assets` |
   | `tagpu_posedraw.c` | `tagpu_posedraw_handover` |
   | `tagpu_terr.c` | `tagpu_terr_handover` |
   | `tagpu_hires_draw.c` | `tagpu_hires_handover` |
   | `tagpu_mark.c` | `tagpu_mark_handover` |
   | `tagpu_fx.c` | `tagpu_fx_handover` |
   | `tagpu_shadow.c` | `tagpu_shadow_handover` |
   | `tagpu_feat.c` | `tagpu_feat_handover` |
   | `tagpu_native.c` | `tagpu_native_worldtgt` |
   | `tagpu_gaf.c` | `tagpu_gaf_mip_bytes`, `tagpu_gaf_mip_chain`, `tagpu_gaf_mip_off` |
   | `tagpu_posebake.c` | `tagpu_posebake_geom_mirror`, `tagpu_posebake_mat_mirror` |
   | `tagpu_scaffold.c` | `tagpu_scaffold_overlay` |
   | `tagpu_hires.c` | `tagpu_hires_verts_want` |
   | `tagpu_text.c` | `tagpu_text_atlas`, `tagpu_text_dims` |
   | `tagpu_fps.c` | `tagpu_fps_quads` |
   | `tagpu_overlay.c` | `tagpu_overlay_draw` |
   | `tagpu_render3do.c` | **nothing** — the only one |

   **So landing 11 is `tagpu_restore_core.c`'s split, sixteen times.** Each of those files is a
   PRODUCER the Vulkan lane depends on with a GL DRAW half bolted to it, and the landing deletes
   the half, in place, leaving the producer and its `_handover`. Not one of them is a file that
   can be removed. The six files the row names are the only ones that go whole, and they are the
   small part.

   `tagpu_native.c:3753` opening `if (!gl_draws) { …hand over…; return; }` and closing at `:3812`
   is what makes this tractable: **everything below that line in the unit pass is GL-only by
   construction**, which is why `tagpu_shadow.c` and `tagpu_hires_draw.c` carry GL with no lane
   guard of their own — their draw entry points are called from below it (`:4155`, `:4463`,
   `:4522`) while their hand-over entry points are called from the Vulkan lane. The seam inside
   each file is real and already drawn; it is just drawn sixteen times.

   The other headers are cleaner than that: `openglshader.h` and `d3d9shader.h` have **no
   includer outside the set at all**; `render_ogl.h` has four (`config.c`, `dd.c`,
   `fps_limiter.c`, `winapi_hooks.c`) and `render_d3d9.h` five (those plus `utils.c`,
   `wndproc.c`), all of them the renderer-selection surface rather than drawing.
   `tagpu_restoreglsl.h` has thirteen, which is the split landing 7 already made.

   **THE METHOD NOTE, because the first pass of this survey got it wrong twice.** Stripping string
   literals before scanning for `#include` **deletes the include paths** — `"render_ogl.h"` is a
   string literal — and the first run reported cheerfully that nothing includes any of these
   headers. And a `\bgl[A-Z]\w*\b` pattern matches `glUp` and `glOff`, two local variables in
   `tagpu_vk_gui.c`, which is how a file with no GL in it appeared in the table. The counts above
   are matched against **the 94 symbols `opengl_utils.h` actually declares**, over sources with
   comments and string literals removed. Same trap as the shader grep over a truncated range, and
   as counting `call` sites without following the control flow: **a mechanical scan is only as
   good as the boundary nobody checked.**

   **The restorer was missing from this list until 2026-09-17 and it is what proved the list was
   a guess.** `tagpu_restoreglsl.c` includes `opengl_utils.h` and calls `glDeleteProgram`, so it
   cannot survive this landing — but before landing 7 split it, deleting it would also have taken
   `tagpu_rglsl_tileable` (called from `tagpu_terr.c:1034` and `tagpu_gaf.c:1130`), the weight
   reader and every job queue, all of which the surviving gather halves need.
   `tagpu_restore_core.{h,c}` is the half that stays and it is **not** in this list.
   Worth checking the rest of the list the same way: a file named here for being GL may carry
   something the gather halves call, and the way to find out is `git grep` on its exports rather
   than on its includes.

   **THE REST OF THE LIST, CHECKED THAT WAY [SURVEYED 2026-09-18, RE-DONE THE SAME DAY AFTER THE
   FIRST PASS GOT ONE OF ITS HEADLINES WRONG]. It is four landings, not one.**

   **The method, because the first pass used a worse one.** `git grep <symbol>` counts hits in
   comments, in string literals and in declarations, and this entry's own warning — *read the
   hits, not the count* — was written and then not applied. The check that works is: strip block
   comments, line comments and string literals from each `.c`, then match the **bare symbol**
   (`\b<sym>\b`, not `<sym>\s*\(`) — the bare form because a function-POINTER use blocks a
   deletion exactly as a call does, and `g_ddraw.renderer == ogl_render_main` is precisely that.

   * **`opengl_utils.c` — goes WITH `render_ogl.c`, and the first pass said something wrong and
     more interesting.** It claimed the four shader helpers were "referenced only by
     `opengl_utils.h` — already dead". They are not dead: `oglu_build_program`,
     `oglu_build_program_from_file`, `oglu_ext_exists` and `oglu_init` are all used by
     **`render_ogl.c`**, which the first pass had excluded as "not surviving" and then reported as
     absence. Outside the deletion set only **`oglu_load_dll`** is used (`dd.c`). The practical
     consequence is better than "dead" and different from it: **delete `opengl_utils.c` and
     `render_ogl.c` together and nothing outside the set loses a helper**, with `oglu_load_dll`'s
     one caller the only thing to resolve.
   * **`render_d3d9.c` — four surviving files, not one.** Seven exports; real users in `dd.c`,
     `utils.c`, `winapi_hooks.c` and `wndproc.c`. `d3d9_release_resources` has **no user outside
     the file at all** (confirmed on the re-check). No lane involvement.
   * **`render_ogl.c` — three surviving files, AND A NEGATIVE RESULT THAT A CARELESS GREP WOULD
     HAVE INVERTED.** `ogl_create` (`dd.c`), `ogl_release` (`dd.c`, `winapi_hooks.c`) and
     `ogl_render_main` (`dd.c`, `fps_limiter.c`, `winapi_hooks.c` — as a **function pointer**,
     `g_ddraw.renderer == ogl_render_main`, which a call-shaped pattern misses entirely).
     **`render_vk.c` and `tagpu_vk.c` both match a grep for those names and every hit is PROSE** —
     comments describing what the GL backend does. **The Vulkan lane uses nothing in
     `render_ogl.c`.** This one survived the re-check unchanged, because it was the one place the
     first pass actually read its hits.
   * **`tagpu_restoreglsl.c` — BLOCKED, for the ordinary reason. [THE FIRST PASS'S HEADLINE HERE
     WAS FALSE AND IS WITHDRAWN.]** It said *"and `tagpu_vk_restore.c`, a VULKAN file, which calls
     `tagpu_rglsl_mips` — so the dependency is not 'the surviving gather halves still need it';
     the Vulkan restorer does."* **That hit is a COMMENT** (`tagpu_vk_restore.c:2046`, prose
     explaining that the GL lane refuses an odd mip level *"for the same reason
     (tagpu_rglsl_mips)"*). **No Vulkan file uses this module at all.**

     Re-checked with comments and strings stripped: all **eleven** exports have real users, and
     the users are **four** GL-side files — `tagpu_gaf.c`, `tagpu_terr.c`, `tagpu_native.c`,
     `tagpu_gui_surf.c`. (`tagpu_render3do.c` and `tagpu_restore_core.c` were comment and
     declaration hits.) The file is 291 `gl[A-Z]` sites deep and includes `opengl_utils.h`, so it
     cannot survive as it stands. **Landing 7's split into `tagpu_restore_core.{h,c}` is not
     finished**, and finishing it is a landing of its own that has to come before this one —
     which is what this entry already suspected, and the suspicion needed no Vulkan file to be
     true.

   **NOT CHECKED:** `openglshader.h` and `render_ogl.h` — headers, which can only be checked by
   include, which is the check this entry warns against relying on.

**A twelfth thing that is not a landing: the stand-downs are session-latched.** §2.35 measured it
— a pass that has refused once stays dark for the process even after the condition clears. Every
landing above should either clear its own latch when the condition does, or say why it cannot.

## What the seam has to grow, and what the opt-out becomes

**The offscreen world target breaks "one colour attachment in the swapchain's format".** The
world passes need colour+depth at `ss×`, resolved before the UI composites at 1× — the order
`ogl_render` already uses with `tagpu_overlay_target_fbo()`. Constraint 3 needs rewording, not
breaking: presentation stays one file; what changes is that the seam owns two attachments'
worth of targets rather than one.

**`renderer=` becomes the master opt-out** and `tagpu_overlay.off` retires. It cannot mean what
its name says: `tagpu_overlay.c:7` calls it *"the kill switch for everything we draw in GL"*, but
the own-the-draw suppressors install from `dllmain.c:102` on their own levers, so the switch
cannot uninstall them. What it CAN leave behind is narrower than this paragraph used to say —
see the survey below: the per-draw skips all ask a lane-published flag and go inert on their own,
and what survives `tagpu_overlay.off` is the pair of `je`->`jmp` flips, which have no flag to
answer.

**That trap is real, it is landing 10b, and it is ONE PATCH rather than the class this paragraph
first claimed** [surveyed 2026-09-18; the first version of this paragraph is corrected below
rather than deleted, because the way it went wrong is the reusable part].

**What the first version said, and why it was wrong.** It read the arming line a `renderer=gdi`
boot prints —

```
owndraw: ARMED target="all" opaque@0x459830=OK nano@0x459C70=OK buildfx@0x458DD0=OK
structshadow@0x4592C6+0x45952C=OURS shadow@0x459338+0x45958C+0x4594DB=OURS
(engine rasterise skipped for target; writeback must paint it)
```

— and concluded that the engine's rasterise *is* being skipped on that lane. **That trailing
clause is a fixed string in the format, not a report of behaviour**, and "ARMED" means the
detours are installed, not that they skip. Reading a label as a measurement is the same failure
as counting `call` sites without following the control flow, and it is the third time this gate
has recorded it.

**What is actually true.** The detour is installed at `DllMain` — that is the arming rule, and it
does not change — but almost every one of these suppressors decides *per draw*, against a flag
only a live lane sets:

* **The opaque and nano rasterise** (`0x459830`, `0x459C70`) and the **pre-shadow composite wipe**
  ask `tagpu_posedraw_live()`, which is `s_state == 1 && !tagpu_vk_owns_present()` — true only
  after the posed program has linked. `tagpu_owndraw.c` already argues its own safety by
  DIRECTION: *"a stale read can only be stale in the direction of NOT skipping"*.
* **`buildfx` `0x458DD0`** asks `tagpu_native_owns_obj`, which returns 0 unless `s_armed == 1`,
  and `s_armed` is written **only** in `tagpu_native_frame`.
* **`terrown`, `featown`, `fxown`, `markown`, `cursown`** each hold a `volatile unsigned char`
  the stub compares, set to 1 only by a `set_skip(ours-live)` call from the pass that paints,
  with a frame heartbeat that restores the engine's draw if the pass stops.
* And **every one of those flags is set from `tagpu_overlay_draw`**, which has exactly two
  callers: `render_ogl.c:1632` and `render_vk.c:232`. **`render_gdi.c` contains no `tagpu_` call
  at all.** So on the gdi lane no flag is ever set, `s_armed` stays at its initial `-1`, and
  every one of those suppressions is inert by construction rather than by luck.

**The one that is not.** `tagpu_owndraw_init` flips two `je`s to `jmp`s —
`patch_je_to_jmp(0x4592C6)` and `(0x45952C)` — whenever `g_armed && g_all`, and **a byte flip has
no runtime gate to be inert through**. `tagpu_owndraw.h` says what that costs: *"the engine then
draws NO cached slant shadow and the native pass owes every structure one"*. With the shipped
defaults (`tagpu_owndraw.on` = `all`) under `renderer=gdi`, nobody pays that debt, so **every
building loses its slant shadow** and the lane is not stock. That, plus the DirectX-warning byte
named as a residual below, is the whole of the gap.

**AND `renderer=gdi` CANNOT BE DRIVEN OR MEASURED BY OUR OWN TOOLING AT ALL — landing 10c, and
item 11's exit condition depends on it** [found 2026-09-18 while trying to photograph 10b's
effect]. `tacli ui` answers *"no UI snapshot appeared"* on that lane, and the cause is the same
shape as everything else on this page: the whole on-demand **trigger family** —
`tagpu_ui_frame` (`tacli ui`), `tagpu_peek_frame` (`tacli peek`), `tagpu_cat_frame`
(`tacli scenario`'s validation), `tagpu_weapons_frame`, the scenario applier's detection half —
is called from **`tagpu_overlay_draw`** and from nowhere else (each has exactly three mentions in
the tree: its prototype, its definition and that one call). `tagpu_overlay_draw` is called only from
`render_ogl.c:1632` and `render_vk.c:232`.

So on the gdi lane the shell cannot be driven past the main menu, no scenario can be applied, no
memory can be peeked, and **no measurement of any kind can be taken**. That is why this gate has
no picture of a structure shadow returning: the effect is visible only where the engine draws the
world, and that is the one lane the instruments do not reach.

**It is a landing of its own (10c) and it is tooling, not rendering.** The fix is to call the
trigger family from a lane-independent point — the game-thread flip hook `0x4C63A0` already runs
on every renderer (`gui: first flip on thread 652` appears in a gdi boot's log) and is the
obvious host. **The design risk that looks biggest is not there**: the trigger functions take a
`const TAGPU_FRAME*` only to throttle themselves — `if (f && (f->frame_counter % 5))` is the whole
of their use of it, in both `tagpu_gui_snap.c:613` and `tagpu_cat.c:306` — and every one
null-checks it, so a game-thread host can pass its own flip count or `NULL` with no frame packet
in existence. What DOES need thought is that they would move from the render thread to the game
thread, and they read engine memory. Note for whoever takes it: `render_gdi.c` currently contains **no** `tagpu_` call at
all, and landing 10b's safety argument quotes that fact, so if the triggers are added there
instead, 10b's note has to be re-read rather than assumed to still hold.

**So landing 10b is: give the structure-shadow pair the gate every other suppressor already
has.** It moves a byte patch, so it reviews at `high`; it is its own landing because putting a
byte patch in a diff dominated by deletions is where a wrong one hides; and **11 is blocked on
it**, because 11's headline claim is that `renderer=gdi` becomes the documented stock reference
and that claim is false by two bytes until 10b lands.

**What the gate turned out to cost: two review rounds, both spent on the same mistake in
different clothes.** Writing the gate was an afternoon; getting its *predicate* right took a
review and a re-review, and both rounds found the predicate asking a question that was not the
one that matters. The sequence is worth carrying into 10c and 11, because it is a reasoning
failure and not a coding one:

| round | predicate | what it asked | why it was wrong |
|---|---|---|---|
| written | `s_armed == 1` | is the pass armed | every other suppressor asks *will anything paint it*; this asks a different question |
| review | `tagpu_classicpp_on()` **or** `(gl_draws && tagpu_posedraw_live())` | is a painter configured | the two painters are **not alternatives** — both draw out of `pdu[]`, which needs `tagpu_posedraw_ready()`, so with the posed program refused the either/or raised the gate over an empty frame **in the shipped default** |
| re-review | `s_armed == 1 && gl_draws && s_ssPainter` | did anything actually paint one last frame | — |

The fix was to stop predicting from levers and publish an observation from the painters
themselves (`tagpu_native.c`'s `s_ssPainter`, cleared on read), which bounds every stale direction
at one frame by construction. **Two claims this plan made were disproved on the way** and are
corrected where they were written: the Vulkan lane does **not** paint a structure's slant through
`tagpu_shadow_handover` — `tagpu_shadow_begin`/`_end` are called only below the `!gl_draws` return,
so `s_pubHave` is never set on that lane and `tagpu_vk_shadow.c:788` stands down — and
`tagpu_classicpp_on()` is a lever, not a statement that the cast-shadow map drew.

**The transferable part for landings 10c and 11:** a lever tells you what a human asked for, not
what the frame did. Where a suppressor's safety depends on a painter having run, ask the painter.

The per-pass `tagpu_<x>.off` files, `tagpu_defaults.off`, `tagpu_reclaim.off` and
`tagpu_curs.off` are unchanged.

**One residual is named rather than fixed:** `tagpu_apply_patches()` is called unconditionally at
`dllmain.c:75` and its first act patches the DirectX version warning at `0x4266A7` with no lever
at all. `README.txt`'s claim that three files give you *"the stock one through cnc-ddraw"* is
false by that one byte, and stays false unless it is given a lever or the sentence is corrected.

## What this plan does not know yet

* **Whether the audit reorders all of it.** Three gaps came out of one blind spot while planning;
  a fourth and fifth would not be a surprise.
* ~~**What `otherDraws` actually costs in play**~~ — **ANSWERED, and it was landing 6's to answer,
  not landing 4's [2026-09-17].** The cost was measured as TOTAL (a building placement blanks every
  posed unit) and landing 6 closed it ([gpu-status](gpu-status.html) §2.41). This bullet sized
  landing 4 against a stand-down that no longer exists, so it no longer bears on landing 4's shape.
* **Whether the restorer's frame-sliced budget survives the port.** 47 attachments' worth of
  state at `NK=4` (`tagpu_restoreglsl.c:47`), MRT over layers of a ping-ponged 2D-array texture,
  against a GPU-millisecond budget. The shaders are the easy half.
* **Whether the gadget dispatcher can be re-entered safely** — **ANSWERED FOR ONE ENTRY, AND
  ONLY OVER TWO SCREENS [landing 9, 2026-09-18].** The entry that re-renders a whole screen is
  `GUI_StageUpdateDraw 0x4A81E0(gi, 0x40)`, and it is now documented
  ([exe-reverse-engineering](exe-reverse-engineering.html), [gui-gadgets](gui-gadgets.html)).
  Three facts make the re-entry safe by construction rather than by luck, and they are the
  answer to "calling a 1997 UI builder twice":
  * **The `0x40` path allocates nothing and frees nothing.** Both allocators sit behind
    `0x4A82F0 and eax,1` / `0x4A82F7 je 0x4A90D1`, which jumps past them; all **six** free sites
    sit inside `0x4A950A test bl,0x2 / je 0x4A95C2`. A redraw cannot leak and cannot double-free
    because it reaches neither.
  * **The engine issues our exact call itself.** `GUI_Pop 0x4A9660` calls `0x4A81E0(gi, 0x40)` at
    `0x4A96BF` whenever the popped screen's flags carry `0x800`, and the dirty-flag pump
    `0x4A9FD0` issues one at `0x4AA0CD`. We are not inventing a re-entry; we are making one the
    engine already makes.
  * **It is guarded at its own head.** `0x4A81EA` loads `TheActive_GUIMEM` and returns at
    `0x4A81FF` when it is NULL, and we check `panel+0xBC` before calling because a NULL
    destination resolves to the PRIMARY surface inside `0x4C6B70`.

  **What is still not known is the eight-screens-and-the-ninth part.** Two screens were
  exercised — `MAINMENU.GUI` (115 ops) and the in-game `ARMMAIN2.GUI` (1 op). The shell's other
  screens, and any screen whose gadgets own engine state, are untested.
* **What a failed Vulkan bring-up should show.** The GL path falls back to `gdi` with a driver
  warning (`dd.c:1998`); the same shape is the obvious answer, but a player then gets stock TA
  with no patch, and nothing decides today whether that is silent.
* **Drift.** The previous-build A/B is a relative bar by construction; nothing in this plan
  catches ten landings of 0 px that together moved the frame. **What landing 10b did establish is
  the instrument this would need**, which nothing here had: a scene that reproduces ACROSS BOOTS,
  not merely within one. `scenario apply exit-sort` on `renderer=vulkan` places its five units at
  a camera reproduced to the digit and gives **1 535 colours and 0–1 differing pixels** between
  boots and between builds, over the frame **below y = 130**. Two conditions make it work, and
  both were learned the hard way here:
    * **exclude the message band.** The fixture clears units, each death writes a log line, and TA
      words each one at random — *"vermin have been exterminated"* against *"forces have been
      obliterated"* — so the whole frame differs by ~5 200 px of pure text with an identical world
      beneath it.
    * **do not use a colour count, and do not use the shell.** The same build, fixture and camera
      gave 2 727 colours on one boot and 498 on another as the map revealed; the shell varies
      against *itself* by 181–191 px.

  So an absolute baseline is now takeable — a stored frame of that scene, re-diffed each landing —
  where before this it was not clear any scene was stable enough to store. Nobody has taken one.
* **S3 has still never run.** `roadmap.md`: *"the `_local` test VM is being built; nothing
  measured yet"*. Every figure in Phase G is one GPU, under Wine, at one `ss`.
