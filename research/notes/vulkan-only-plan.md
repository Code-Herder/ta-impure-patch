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

   **STARTED 2026-09-17: the generator reads the header and builds every variant, and the
   TRANSFORM is what is not ready.** `spirv-gen.py` now carries the variant manifest,
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

   Both are the **tool's to learn rather than the shader's to reformat**: that header is the one copy
   of the text and is shared with `tools/tascene`'s browser pack, and reflowing it for a generator's
   convenience is what its own header forbids. `RESTORE_READY = False` keeps the restorer out of
   `SOURCES` and `PROGRAMS` until the same change teaches `transform` both shapes, so the build's
   shader gate is unaffected meanwhile. `tools/glslang-fetch.sh` has been run (16.6.0, pinned by
   hash), and regenerating with the edited tool moved **only the `transform` hash line** in all
   eleven committed headers — no SPIR-V word changed, which is the proof the edit touched no shader.

   **It is not a blocker and nothing stands down for it today** — landing 2 mirrored the restored
   atlases, so the lane draws restored art with the restore itself still running on GL. Landing 7
   is owed to the END STATE rather than to any present refusal, which is why it sits behind 5 and 6
   and ahead of 4.
8. **`PK_PIXELS` closed.** `tagpu_gui_hook.c:330`'s op kinds `OP_LINE`, `OP_BAR`, `OP_RECT`,
   `OP_FRAME` and `OP_SCALE` publish through `pub_surface_bytes` at `:1489` — *the engine's
   surface bytes as they stand at the flip*. They become drawn geometry with their own packet
   kinds. Oracle: `strict`.
9. **Seeds carry art.** A `PK_SEED` is published lazily on first touch
   (`tagpu_gui_hook.c:1289/1300/1332`) because we cannot know how a surface got its contents.
   The fix is to make the engine redraw: `gui-renderer.md:55` has the panel as a pre-rendered
   surface at `panel+0xBC` whose gadget handlers sit behind the type dispatcher **`0x4A9176`**
   with table **`0x4A962C`**. What is missing is the per-screen "render every gadget" entry and
   whether it is safe to call twice. Force one repaint after arm and after a level-change reset
   and every pixel arrives as an op — which closes G15e's *"seeded art stays indexed until
   repainted"* as a consequence rather than as a special case.
   **Rejected:** submitting the seeded surface to the restorer as a job. Its contract
   (`tagpu_restoreglsl.h`) is *a frame of art from an R8 atlas with its colour key*; a seeded
   panel is a composite of art, glyphs and chrome with no key and no tileability, the model was
   not trained for it, and `uidiff` has no cell to match it against.
10. **The engine-frame fallback layer goes** from the composite. `tagpu_gui_surf.c:51` has it as
    the bottom of three; G15b, G15c and G17d measured **0 holes** across 120 stops, so nothing
    reads it.
11. **The deletion landing** — `render_ogl.c`, `render_d3d9.c`, `opengl_utils.c`,
    `openglshader.h`, `render_ogl.h`. `renderer=gdi` becomes the documented stock reference.

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
the own-the-draw suppressors install from `dllmain.c:102` on their own levers and never ask
whether a lane is alive, so the switch leaves the engine's rasterise skipped with nothing
replacing it.

**That trap is not GL-specific and must be fixed first.** Set `renderer=gdi` today with the
shipped defaults and `tagpu_owndraw_init` still detours `0x459830` / `0x459C70` — the "stock
reference" lane is not stock. A suppressor must not arm unless something is going to draw what
it suppresses, by a handshake rather than by ordering luck. Without that, no opt-out in this plan
is honest.

The per-pass `tagpu_<x>.off` files, `tagpu_defaults.off`, `tagpu_reclaim.off` and
`tagpu_curs.off` are unchanged.

**One residual is named rather than fixed:** `tagpu_apply_patches()` is called unconditionally at
`dllmain.c:75` and its first act patches the DirectX version warning at `0x4266A7` with no lever
at all. `README.txt`'s claim that three files give you *"the stock one through cnc-ddraw"* is
false by that one byte, and stays false unless it is given a lever or the sentence is corrected.

## What this plan does not know yet

* **Whether the audit reorders all of it.** Three gaps came out of one blind spot while planning;
  a fourth and fifth would not be a surprise.
* **What `otherDraws` actually costs in play** — how often a shipped configuration puts a ghost
  or more than 512 posed units on screen, and therefore whether landing 4 is small or is the
  whole unit pass reopened.
* **Whether the restorer's frame-sliced budget survives the port.** 47 attachments' worth of
  state at `NK=4` (`tagpu_restoreglsl.c:47`), MRT over layers of a ping-ponged 2D-array texture,
  against a GPU-millisecond budget. The shaders are the easy half.
* **Whether the gadget dispatcher can be re-entered safely.** `0x4A9176` and `0x4A962C` are
  documented; the entry that re-renders a whole screen is not, and calling a 1997 UI builder
  twice is the kind of thing that works for eight screens and corrupts the ninth.
* **What a failed Vulkan bring-up should show.** The GL path falls back to `gdi` with a driver
  warning (`dd.c:1998`); the same shape is the obvious answer, but a player then gets stock TA
  with no patch, and nothing decides today whether that is silent.
* **Drift.** The previous-build A/B is a relative bar by construction; nothing in this plan
  catches ten landings of 0 px that together moved the frame.
* **S3 has still never run.** `roadmap.md`: *"the `_local` test VM is being built; nothing
  measured yet"*. Every figure in Phase G is one GPU, under Wine, at one `ss`.
