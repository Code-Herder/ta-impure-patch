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
3. **The caster stream** — the native 3DO stream and the replacement meshes, so the cast-shadow
   map can be drawn on this side of the seam and the passes that sample it stop standing down.
4. **`render_vk.c`** — the fourth backend, `renderer=vulkan`, present into `g_ddraw.hwnd`, the
   offscreen world target at `ss×` with its resolve, TA's surface uploaded by the backend instead
   of by the GUI pass. Route D's window, `tagpu_vk_wndproc`, `WM_TAGPU_VK` and the geometry
   tracking are deleted here.
5. **`tagpu_vk_mark.c`** — bars, cursor, band box, digits. The SPIR-V exists
   (`inc/spirv/tagpu_mark.spv.h`); the GL twin is the oracle and goes in the same landing.
   **After** 2 and 3, because until then it would draw over a world that is not there.
6. **The build ghost and the `otherDraws` stand-down** (`tagpu_vk_unit.c:1316`). Not reached
   today — the unit pass refuses on the atlas mirror several checks earlier — so its cost is
   still unknown.
7. **The restorer**, `tagpu_vk_restore.c`. Ported **unchanged**, which needs `spirv-gen.py` to
   emit four variants: `NK ∈ {1, 2, 4, 8}` (`tagpu_restore_glsl.h:10`), `WMAX = NK × kmax`
   (`tagpu_restoreglsl.c:472`). Spec constants cannot do it — `#if NK > 1` declares a different
   number of `out` locations and SPIR-V interface variables are static. Landing 2 makes the lane
   *usable* with Classic++ on; this is what moves the restore itself off GL.
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
