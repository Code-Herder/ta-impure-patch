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

Deleted (`render_d3d9.c` and `render_ogl.c` ✓ gone 2026-09-18, the rest still standing):
`render_ogl.c` (2 015 lines), `render_d3d9.c` (742), `opengl_utils.c`,
`openglshader.h`, `render_ogl.h`, `tagpu_abshot.c`, every `tagpu_*` GL draw, and
`tagpu_overlay.off`. **`openglshader.h` was struck off this line on 09-19 and is back on it
since the clean cut** — its two shaders were the build-time source for `tagpu_vk_surf.c`'s
bottom-layer program, and that program is deleted (item 11's survey says why, and what the
near-miss taught).

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

**THIS SECTION WAS WRITTEN BEFORE GATE 4 AND IS STALE IN BOTH DIRECTIONS — re-checked
2026-09-18 while scoping 11-3.** Both passes it lists as having no twin now have one, and the
one real gap it describes is described wrongly. Corrected below; the original text is kept
underneath because the reasoning in it is still how the question gets asked.

**Passes with a Vulkan twin (re-checked):** scaffold, unit, shadow, terrain, features, effects,
GUI, fps, **and `tagpu_mark.c` (`tagpu_vk_mark.c`, which calls `tagpu_mark_handover`) and the
Classic++ restorer (`tagpu_vk_restore.c`, 136 KB)**. Fourteen `tagpu_vk_*.c` files exist.

**THE REAL GAP IS THE BUILD GHOST, AND IT IS A PRODUCER GAP RATHER THAN A PASS GAP.**
`tagpu_vk_unit_record_ghosts` exists and is called (`tagpu_vk.c:2523`); `tagpu_posedraw.c:964`
carries `r->ghost` through the hand-over; `tagpu_posedraw.c:887` says in as many words that
*"THE BUILD GHOST IS RECORDED SINCE LANDING 6 and no longer counted"* as an `otherDraws`
refusal. **But nothing ever records one on the Vulkan lane.** `q.ghost = 1` is assigned in
exactly one place in the tree — `ghost_one` — which is reached only from `ghost_pass`, which was
called from `tagpu_native.c:4676` on `main`: **below** that function's `if (!gl_draws) { …hand
over…; return; }` at `:3875`. So the ghost pipeline on the Vulkan side is a consumer with no
producer, and **the build ghost has never appeared on that lane**.

**AND LANDING 11-2 IS WHERE THAT STOPPED BEING ACADEMIC.** `auto` and `openglcore` selected the
GL lane until then, so the build ghost — `tagpu_ghost.on` is a play default — worked in the
shipped configuration. 11-2 pointed those spellings at Vulkan, so **the default experience lost
the build ghost, and this is the landing that did it.** It is named here rather than discovered
later: the fix is to lift the ghost's RECORD half above the seam (`ghost_one` is pure — it
fills a `TAGPU_PDUNIT` and calls `tagpu_posedraw_unit`; only `ghost_pass`'s wrapper is GL), which
is landing 11-3's work and the reason that landing is not a pure deletion.

The stale text, kept for its reasoning:

> **Passes with a Vulkan twin:** scaffold, unit, shadow, terrain, features, effects, GUI, fps.
>
> **Passes with none:**
>
> * **`tagpu_mark.c`** — health bars, cursor, band box, group digits. 19 GL binds, drawn at
>   `tagpu_native.c:4294`. **Its shaders are already translated** — `inc/spirv/tagpu_mark.spv.h`
>   exists; G19c did it and only the pass file was never written.
> * **`tagpu_restoreglsl.c`** — the Classic++ restorer. The one module with no SPIR-V, excluded
>   from G19c because its GLSL is not fixed at build time.

**And two stand-downs that make the gap wider than "two passes".** `tagpu_vk_unit.c:1316`:

    if (h.otherDraws > 0) { ... goto standdown; }

`otherDraws` counts units the GL twin drew that the hand-over does not carry. When it is
non-zero the unit pass draws **nothing at all**.

**The build-ghost half of this is FIXED and the text above was stale** [re-checked 2026-09-18]:
`tagpu_posedraw.c:887` records ghosts instead of counting them since landing 6, and in any case
`ghost_pass` never ran on the Vulkan lane to count anything. What remains true is the ceiling:

* any frame with more than **512** posed units — `TAGPU_PD_MAXHAND`, a deliberate ceiling under
  `MAXU`'s 2048, chosen because each unit costs a 14 336-byte uniform window per frame slot —
  and the two other counted cases, an arena that would not grow and a unit with no pieces.

Not visible from the configuration Phase G was measured in.

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

One pass per landing, each with its own measurement, never as one drop — the lane's standing rule.
**Each landing deletes its own pass's GL draw in the same diff**, once the figure is banked.

### 1–10 — all landed, 2026-09-16 to 2026-09-18

The per-landing write-ups are [gpu-status](gpu-status.html) §§2.35–2.62 and that is the durable
record; what follows is the index and, below it, the things these ten gates established that
outlive them.

| # | what | landed | § |
|---|---|---|---|
| 1 | the shipped-configuration audit (no code) | 09-16 | 2.35 |
| 2 | the restored-atlas CPU mirror — feat/fx/terr | 09-16 | 2.36 |
| 3a/3b | the caster stream: the unit atlas's mirror, then the replacement meshes | 09-16/17 | 2.37, 2.38 |
| 5 | `tagpu_vk_mark.c` — bars, cursor, band box, digits, order lines and labels | 09-17 | 2.39 |
| 5b | the lane's frame was upside down | 09-17 | 2.40 |
| 6 | the build ghost and the `otherDraws` stand-down | 09-17 | 2.41 |
| 7 | the restorer: the core/GL split, the Vulkan half, four consumers, the mip rule | 09-17 | 2.42–2.47 |
| 4a–4d | `render_vk.c`: the backend, the driver, the `ss×` target, the deletion of route D | 09-17/18 | 2.48–2.57 |
| 8a/8c | `PK_BAR` for `OP_BAR`, and `OP_LINE` split axis-aligned/diagonal | 09-18 | 2.58, 2.60 |
| 9 | seeds carry art — `GUI_StageUpdateDraw(gi, 0x40)` at the flip's return | 09-18 | 2.61 |
| 10 | the duplicate engine-frame upload goes | 09-18 | 2.62 |

**The order ran 5, 5b, 6, 7, 4, 8, 9, 10, not 4 first.** Landing 4 deletes route D, and route D's
window is the only place a swapchain can come from while GL owns `g_ddraw.hwnd` — so a same-frame
GL-vs-Vulkan A/B stops being expressible the moment 4 runs, and 5, 6 and 7 all needed one. 4a then
left route D *unreachable rather than deleted*, which gave 4b a better oracle still: one binary
answers both `renderer=vulkan` and `renderer=openglcore` + `tagpu_vk.on`, so a pass is measured as
a same-build two-configuration pair rather than against a previous build.

**8, 9 and 10 built the UI layer, and THE CLEAN CUT has since deleted the half of it that drew.**
The op stream those landings published — `PK_BAR`, `OP_LINE`'s split, the seed repaint, the
sprite atlas — is still captured by `tagpu_gui_hook.c`; the twin store, the composite and the
`LAY_FS` machinery that consumed it are gone. Read anything below about a twin or a layer as
history. What stays useful is the ENGINE knowledge, which is in
[exe-reverse-engineering](exe-reverse-engineering.html) and [gui-renderer](gui-renderer.html).

### What these ten gates established that outlives them

**A refusal placed above the code that would satisfy it refuses for ever.** A pass that returns 0
skips its own resize and its own upload, so it waits permanently for the thing the refusal was
meant to wait for. Landing 2 hit this three times in one file, each fault hidden behind the one
before it. And a refusal must test **the view the descriptor names**, not a hand-over saying a
mirror exists somewhere.

**A serial, and what a "rows" number means on each side of the seam.** Landing 2 stored the rows
it SENT and compared the rows PUBLISHED, so the two could never be equal and a 32 MB upload ran
every frame; landing 3a made the same mistake with the opposite sign and never took its partial
path. Neither shows in any capture. Every hand-over here crosses rows.

**A dimension that can move is an image LIFETIME problem.** Rebuilding an image when a published
value changes is `kill_image` on an image other slots' submitted command buffers still name. The
fix is to make the moving value *refuse at the producer* so the rebuild is unreachable — a pass
that cannot reproduce a frame draws nothing rather than a smaller version of it.

**Per-frame-slot staging is not per-batch staging.** Landing 7c and then 7d hit this one resource
apart: `tagpu_rcore_step` can issue more than one batch in a slice, so a second batch overwrote
the first's vertices (7c) and its parameter tables (7d) before either draw ran. Batches per slice
is a *time budget*, so no arena size fixes it — `vkCmdUpdateBuffer` puts each batch's data in the
command stream at the point of its own draw. Cost: six unpainted cells in a 46 MB atlas, and 118
of 1 304 feature frames restored through another frame's colour key.

**0 px from a fixture that never took the branch is worse than no measurement, because it is
reported as a pass.** Landing 3a's first version armed `native.on` alone, which measures the
FALLBACK rather than the restored art. Before quoting a restored-art figure, check the twin
actually painted (`tagpu_restoredump.on`, or `cmp` the captures from `assets=1` and `assets=0`).

**And do not measure on a small fixture.** Landing 3b found **65 px of 209 814 differing on a
257-unit crowd with everything this plan has built turned OFF** — the two rasterisers disagreeing
about which triangle owns a pixel an edge passes through. It was invisible for six gates because
every A/B had been taken on four units, where no edge lands on a sample point. 0.031 % of drawn
pixels, symmetric, characterised but **NOT traced**, and it needs a landing of its own (§2.38).

**Grepping for `gl[A-Z]` finds one shape of GL-coupled function and is blind to the other.** Four
functions across 4b-2 and 4b-3 read as pure GL executors and were not: they PUBLISH the hand-over
the Vulkan twin runs from (`tagpu_posedraw_live`, `draw_layer`'s `s_mHand`, `sharp_begin`'s
`s_sharpOn`/`s_sharpInk`, `sharp_minimap`'s CPU bake). Head-returning them built cleanly, ran
cleanly and handed the twin nothing. The second grep is for what the function publishes.

**Before sizing a port by counting GL calls, ask which of them the lane can still reach.**
`tagpu_posedraw.c`'s 150 GL calls needed no gate at all — every entry point is reached through the
composite, which the Vulkan lane exits before.

**Enumerate every branch AND every return before the point of interest, and ask where each lands.**
Landing 9's first version looked for early returns before an allocation, found one, missed three
forward jumps past it, and concluded the opposite of the truth. The same failure produced "three
fills" for a function that shades (`0x4BF4D0`), and a shader range computed by a heuristic that
truncated a multi-line C string at the first line ending in a semicolon — which made `uSurf` read
as unused when it has two `texelFetch`es (landing 10). **A mechanical count over a boundary nobody
checked.**

**Name a thing from the engine function, not from the observer's record.** `OP_FRAME` was filed as
"three fills" and shades; `OP_RECT` as "the volume" and is 99.7 % a tint; `OP_SCALE` as "a scaled
blit" and is a textured triangle (`0x4C7580 GAF_DrawTransformed`). Every leaf was recorded as a
rectangle because a rectangle is what the census needed, so the name describes the RECORD.

**An A/B's unlink belongs in the same statement sequence that latches the claim**, not beside the
capture. `vk_present` is not on the path from the latch — the lane returns before it through the
bring-up, on a swapchain rebuild and at ST_FAILED — so an unlink there runs on most frames and is
missed on exactly the frames where a stale capture is likeliest. A guarantee about timing wearing
the words of one about construction.

**When a dependent lane's picture is wrong, measure its INPUT at the moment of use before
reasoning about its arithmetic.** 7e-2's black unit batches were an empty source image, not wrong
maths; the oracle's `SOURCE: IDENTICAL` line compares after everything has settled and could not
see it. Making the OUT shader report the index it had read is what found it.

### The facts about the CODE that these gates settled

* **The GL lane's `ss` numbering is the opposite way round from the guess**: `s_fbo`/`s_colTex` is
  the **1×** pair, `s_fbo2`/`s_colTex2` the **`ss×`** one. The resolve is a LINEAR quad from
  `s_colTex2` into `s_fbo`, and `devres` can raise `ss` to `ceil(k)` and skip the resolve
  entirely. There is a **second, DEPTH** resolve when `selAt1x` (`GL_NEAREST`, the only filter a
  depth blit takes) so 1× selection rects stay occluded — **the Vulkan lane has no answer for it.**
* **Route E and route F, measured** (roadmap §G19a): Vulkan alone on the game window with no GL
  context and no pixel format ever reaches the screen (98.5 %, the rest is the border), and GDI on
  the same window afterwards also reaches it — so **the GDI fallback survives the surface** and
  `render_vk.c` may hand the session over at any point. winevulkan's `HWND` takeover is specific to
  GL's drawable, not to the window.
* **One lane is cheaper than two, and that is all the VA numbers support.** GL alone 39.2 MB
  committed peak, Vulkan alone 37.7–39.5, both 40.6–43.1. The two single-lane ranges overlap, so
  *"Vulkan is cheaper than GL"* is **not** a claim this supports.
* **Image space is a third case the `flipok` table did not have.** In both APIs the fragment y is
  the target's ROW INDEX and NDC −1 is row 0; the conventions disagree only about which end is
  visually *up*, which matters only to a pass that speaks the SCREEN's y. The restorer never does.
* **The restorer's variant is an (`NK`, `kmax`) PAIR and the cross product does not collapse.**
  `nk = MAX_UNIFORM_BLOCK_SIZE / (kmax × 64)` clamped and rounded down, so compiling one `NK` at
  the largest `kmax` overflows the limit that chose it. `tiny` is kmax 56, `full` is kmax 148, read
  out of the weight binaries by `restore_kmax()` — **which is what makes retraining fail the build
  instead of shipping a block of the wrong length.** The decision the generator asked for, stated:
  *the Vulkan lane restores with the two shipped models and no others.*
* **The restorer's device prerequisites hold through winevulkan, measured in the game's own
  process** — 8 colour attachments, 65 536-byte uniform range, 2 048 array layers, usable
  timestamps on the submitting queue family, and all three float/unorm formats as colour
  attachment + sampled + linear. The lane settles at NK=4, the same NK GL picks for the same model.
* **`minUniformBufferOffsetAlignment` is 64 and the weight loader does not check it.** Every group
  offset of both shipped models is 64- and 256-byte aligned, but by construction of the exporter:
  `load_weights` validates `kstride & 15` and `offset`'s bound, never `offset`'s alignment. **GL's
  `glBindBufferRange` has the identical requirement, so this is a pre-existing hole in a shipping
  path** — one line, `(offset & 15)`, beside the check already there.
* **The driver's mipmap reduction is an unweighted 2×2 box average of RGBA**, measured against six
  candidates: alpha comes back exact, every integer rounding lands within ±1 per RGB channel.
  7e-1 replaced it on both lanes with the exact integer `(sum + 1) / 4` in one shader string, so
  the chain is byte-identical by construction rather than by a driver's rounding rule.
* **`GUI_StageUpdateDraw 0x4A81E0`'s `0x40` redraw allocates nothing and frees nothing**, reaches
  every gadget, and is a call the engine already makes itself (`GUI_Pop 0x4A9660` at `0x4A96BF`,
  the dirty pump `0x4A9FD0` at `0x4AA0CD`). The allocations are behind the `0x1` BUILD bit and all
  six frees behind `0x2`. Full disassembly in
  [exe-reverse-engineering](exe-reverse-engineering.html).
* **`uSurf`, `uEng` and 4c-1's bottom layer were three different things called "the engine's
  frame"**, which is why landing 10's entry went wrong twice. All three are gone with the cut.

### Still open out of these ten

* **The 65 px rasteriser-edge difference** (3b, §2.38) — characterised, not traced, needs its own
  landing, and nothing may be measured on a small fixture again.
* **`selAt1x`'s depth resolve**, the HUD-scale shift, and GL's two-step resolve — 4c-2's
  *Not covered*, and `devres` / `k != 1` are untested either way.
* **The GL twin's lines are the WRONG half.** At `ss=2` the driver clamps an aliased line's width
  to 1, so `glLineWidth(ss)` draws pixel-identically to `glLineWidth(1)` and the GL laser is a
  half-lit smear; the Vulkan lane has `wideLines` and gets its 2 px. ~100 px on lasers, 34 on order
  lines, every one of them a pixel GL left black. **Left alone — making them agree changes a
  shipped picture and is the owner's call** (escalation reason 1).
* **`tagpu_vk_restore_job_repalette` and `..._job_clear` have no callers** while the GL side's do
  (`tagpu_gaf.c:1050` and `:272`), so on `renderer=vulkan` neither the atlas clear nor the palette
  follow happens. `repaint`, the blank counter, the cleared-picture flag and the out-of-memory
  list drop are all unexercised.
* **The record-time latch that made 7e-2's units black exists in `tagpu_vk_feat.c` and
  `tagpu_vk_fx.c` too** — unreachable there rather than absent, because neither has a stand-down
  below its staging allocation today. One added re-opens it.
* **The UI atlas has no restore feed at all** since 11-5e-2b removed its read-back (`oglu_load_dll`
  has no caller, so `glReadPixels` never resolved and the mirror refused at its entry guard).
  Nothing regresses, but restoring it is new work (§2.78) — and the cut has since deleted the
  consumer, so it is new work with no client.
* **`PK_PIXELS` is not closed and the gate's exit condition cannot be met as written.** `focus`
  (`0x4BF7B0`, 1 223 310 ops) and `frame` (`0x4BF4D0`) are **destination-dependent tints**: they
  read the pixels they overwrite and remap them through a LUT, so a colour and a box cannot
  express either. `OP_SCALE` (6 967 ops) is a textured triangle needing a vertex buffer and a
  pipeline. Leaving all three as `PK_PIXELS` is defensible — publishing the destination's bytes is
  what a destination-dependent op means — and **that is the owner's decision about the exit
  condition**, not a session's.


11. **The deletion landing** — `render_ogl.c`, `render_d3d9.c`, `opengl_utils.c`,
   `render_ogl.h`, and **`tagpu_restoreglsl.c`**. `renderer=gdi` becomes the
   documented stock reference. (`openglshader.h` came off this line on 2026-09-19 when it was
   found load-bearing, and went back on it on 09-20 when the clean cut removed its last
   consumer — see the survey below.)

   **SPLIT INTO SIX PARTS, 2026-09-18, BY THE SURVEYS BELOW — `landings 11-1 and 11-2 of 6`.**
(Five when it was first written; the sixth appeared the same day when deleting the lane turned
out to belong in front of the pass halves rather than behind them, and `CLAUDE.md` is explicit
that a count which grows is the plan catching up with the work.) The row was
   filed as one item and the work has shown it is not one: the six files it names are ~5 200
   lines, and the GL halves bolted to sixteen passes the Vulkan lane still depends on are
   1 233 more, across sixteen files that each need their own before/after. Per `CLAUDE.md`
   *When a gate turns out to be several landings*, the parts are the seams the survey found, and
   each one has to be something that can be RUN and shown:

   * **11-1 — the renderer-selection surface.** ✓ **DONE 2026-09-18.** `render_d3d9.c` (742
     lines), `render_d3d9.h`, `d3d9shader.h`, and every `d3d9_*` site in `dd.c`, `config.c`,
     `debug.c`, `dllmain.c`, `utils.c`, `winapi_hooks.c`, `wndproc.c` — **2 538 lines deleted
     against 34 added, across thirteen files** (the commit's own totals, 2 539/119 over sixteen
     files, include the three notes; an earlier draft of this bullet mixed the two). It is the one member of the deletion set with
     **no producer half** — nothing in `tagpu_*` calls into it and no Vulkan entry point depends
     on it — so it went whole, and going first shrank the set before the hard part starts. It
     carries this item's DEFINITIONAL half as its documentation: what "stock" is taken to mean
     (next bullet but one).

     **Measured**: `openglcore`, `vulkan` and `gdi` each launch, draw and answer `tacli ui` from
     the main menu; the Vulkan lane still logs its 17 `vk:` bring-up lines. An instance whose ini
     still asks for the deleted lane gets GDI and one log line — `ddraw: renderer=direct3d9 is
     no longer built -- using gdi` — rather than a null renderer, because `renderer=` is read
     from a file the player owns and old ini files outlive the code that read them.

     **THE GAP THIS LANDING DID NOT CLOSE, AND COULD NOT HAVE** [the landing review, MEDIUM].
     `renderer=auto` is the value the generated ini writes, so it is what a player who never
     edits the file gets. Its old body read `if (!IsWine() && d3d9_is_available())` first: on
     **native Windows with a working D3D9 it selected Direct3D9**, and it now selects OpenGL.
     Under Wine `!IsWine()` was already false, so the reference setup's auto path is unchanged —
     which is exactly why none of this landing's measurements touch it, and why they must not be
     read as covering it. **The shipped default therefore changes lane for Windows players and
     has not been run on Windows**; `_local/vm/` is the kit for that and is still waiting on the
     owner. A release that carries this says so in its notes, in the player's terms.

     **Two simplifications that are identities, not judgement calls**, and both are commented at
     the site: `dd.c`'s `BOOL d3d9_active` could only ever become TRUE on the deleted lane, so
     `if (!d3d9_active || g_config.nonexclusive)` is now unconditional and `if (d3d9_active &&
     …) d3d9_reset(TRUE)` is gone — every surviving renderer already took exactly those paths.
     Likewise `wndproc.c`'s two `renderer != d3d9_render_main || nonexclusive` tests.

     **What was deliberately left**: `hook.c:582`'s `_strcmpi(mod_filename, "d3d9")`, which is
     the "don't hook reshade/swiftshader/mesa3d" module blacklist and is about the SYSTEM
     `d3d9.dll` a third-party layer may load, not about our renderer; `ddraw.h`'s four `D3D9Ex`
     mentions, which are the vendored DirectDraw header's own structures; `inc/dd.h`'s
     `FILTER_NEAREST`/`LINEAR`/`CUBIC`/`LANCZOS`, which now have no user in `src/` or `inc/` at
     all; and **`config/ConfigFormUnit.cpp`, the upstream C++ Builder settings GUI**, which is
     not built by the Makefile, still offers "Direct3D 9" in its renderer list and still reads
     and writes the `d3d9_filter` key nothing reads any more. It was already stale against this
     fork before the deletion — its list has no `vulkan` entry either — and if anyone does run
     it, the `renderer=direct3d9` it can write now lands in the fallback above. [The review
     found all four; the first three are one-line facts, the last is a tracked file this fork
     does not build and has not maintained.]
   * **11-2 — the GL LANE, ahead of the pass halves. ✓ DONE 2026-09-18, and this is a
     REORDER of the four bullets below.** They were written lane-last: delete the sixteen passes'
     GL halves first, `render_ogl.c` at the end. That order leaves `renderer=openglcore`
     selectable and drawing nothing for two whole landings — half-done work left behind, which
     the landing bar forbids. Lane-first is also **safe by construction, which lane-last is
     not**: every GL draw is gated on `gl_draws = !tagpu_vk_owns_present()`, the passes run only
     from `tagpu_overlay_draw`, and that has exactly two callers — `render_ogl.c:1632` and
     `render_vk.c:232`. Delete the first and the only caller left is the Vulkan one, where
     `tagpu_vk_own_present()` has already set `s_ownWin` **once at the top of the render thread,
     before the frame loop**, so `gl_draws` is false at every surviving call. The pass halves
     become unreachable by an ordering rather than by a hope, and 11-3/11-4 then delete provably
     dead code instead of live code.

     What went: `render_ogl.c` (2 015 lines), `render_ogl.h`, and the GL lane's call sites in
     `dd.c`, `config.c`, `fps_limiter.c` and `winapi_hooks.c`. `renderer=opengl`/`openglcore`
     now reaches the **Vulkan** lane with a log line, and so does `auto` — which makes the
     plan's "default, shipped" true in the code rather than only in this page. Nothing is probed
     on that path, for the reason the `'v'` arm already gives: `vk_render_main` hands the session
     to GDI late if the lane will not come up, which route F measured as still reaching the
     screen.

     **THE IDENTITIES, each commented at the site.** `dd.c`'s local `nonexclusive` was assigned
     in exactly two places, both under `renderer == ogl_render_main`, and the third term of the
     `opengl_y_align` condition was that same test — so on gdi and Vulkan the extra scanline was
     already never taken and the field already 0; it is now 0 unconditionally, which `render_vk.h`
     wanted. The WGL `SetPixelFormat` block, the two `ogl_create` blocks, the `ogl_release` in
     `dd.c` and in `fake_DestroyWindow`, and `fps_limiter.c`'s Windows-7 `DwmFlush` workaround
     were all inside `renderer == ogl_render_main` tests that no surviving lane satisfied.

     **`tacli shot` MOVED FIRST, so the verb never had a gap** (11-2a). The engine-surface
     screenshot was polled in `render_ogl.c`'s present loop and nowhere else, so it answered on
     the GL lane only — measured failing on gdi and Vulkan by 10c-3's review. It is not a GL
     capture: `ss_take_screenshot(g_ddraw.primary)` reads the fork's own DirectDraw primary, and
     the thread was already proven for it — `keyboard.c:96` and `:102` take the same screenshot
     from the game thread on PrintScreen.

     **The first rehost captured one frame stale, and this landing's review caught it**
     (MEDIUM-1). 11-2a put the trigger poll *and* the capture in the same pass of
     `tagpu_triggers_frame`, which `tagpu_gui_hook.c` hosts from `before_flip`, i.e. at the
     **entry** of the flip `0x4C63A0`. The engine's DirectDraw arm at `0x4C6475` locks the
     primary, draws the cursor, **copies the back buffer onto the primary** with `0x4CBBE0`,
     restores the cursor background and unlocks — all *inside* that call
     ([exe-reverse-engineering](exe-reverse-engineering.html), VERIFIED, and re-checked against
     the disassembly by this landing's review). So at the poll the primary still holds the
     previous frame, and every `tacli shot` was answering with frame N−1.

     **The fix that shipped makes that fact the clock instead of the bug.** The family now
     SERVICES a pending arm before it polls for a new one, so a trigger seen on pass P is
     answered on pass P+1 with a frame the engine presented in between. Both calls are the same
     detour on the game thread, one flip apart: no interlock, no fence, and the only ordering
     claimed is the engine's own copy. The cost is one pass of latency, bounded by the family's
     own 16 ms gate.

     **Two other hosts were tried, and the round-2 review killed both.** They are worth recording
     because both read as correct:

     * **`dds_Unlock`'s `DDSCAPS_PRIMARYSURFACE` branch** shipped for one commit. It *is* after
       the copy — but the branch is gated on `g_ddraw.render.run`, which the **window thread**
       clears on deactivate, minimise, a fullscreen toggle and a mode change, and it is reached
       only when the flip takes its DirectDraw arm (the GDI BitBlt arm at `0x4C63C0` never
       enters it). An arm could wait for an arbitrary later frame, answer with an unrelated
       picture stamped with the wrong time, or never be serviced at all. The safety argument had
       become *the next unlock that happens to pass a flag another thread owns*, which is
       precisely the fix-by-timing CLAUDE.md forbids. **This is the landing's own lesson: the
       first fix for a frame-ordering bug introduced a worse frame-ordering bug, and only a
       reviewer told to attack the synchronisation found it.**
     * **`after_flip`** was rejected, and the reason first written down was **wrong**. It is not
       gated by an `s_opsLive` test — it contains none. The gate is the detour itself:
       `tagpu_detour_observe` emits `test eax,eax; jz` and hijacks the return only when
       `before_flip` returns non-zero, and `before_flip` returns 0 at `if (!s_opsLive)`. So
       `after_flip` never runs without `tagpu_gui.on`, which is the 10c-2 HIGH's shape — the
       right conclusion reached, for one commit, by the wrong mechanism.

     **Measured after the move**: a correct main-menu PNG on `renderer=gdi` (136 215 B, with the
     cursor in it — itself evidence the capture is after the engine's cursor draw) and on
     `renderer=vulkan` (136 143 B). Two shots inside one wall-clock second now produce two files:
     `ss_take_screenshot` disambiguates with a `_2` suffix instead of returning FALSE, which used
     to eat the arm and time the verb out silently. **Not measured**: the single-present case — a
     120-shot sweep through a `scenario load` returned 71 PNGs and 9 distinct frames without once
     catching the loading screen, so whether a screen that presents once is reachable is open.

     **`tascene ab` LOST ITS ENGINE-SIDE CAPTURE and now says so.** It called `tacli glshot`,
     which retired with the lane. `tacli shot` cannot stand in: it captures TA's own DirectDraw
     primary, which on the Vulkan lane holds the engine's frame **without our passes**, so an
     `ab` built on it would compare the browser renderer against a picture our renderer never
     touched. The verb stops with that explanation rather than comparing the wrong images. The
     surviving engine-side capture is the Vulkan one (`tagpu_vk_shot.c` writing
     `tagpu_<pass>_vk.ppm`, compared file-to-file by `vk-ab.py`); **wiring `ab` to it is open
     work this landing did not do.**

     **`tacli glshot` IS RETIRED**, because a GL framebuffer no longer exists in the process. The
     verb is kept and now **fails loudly** with what to use instead, rather than timing out
     silently the way a deleted trigger would. [Its poll was in `render_ogl.c` alone. This page
     and the ta-drive skill said "and in `tagpu_scaffold.c`" for one commit — that file only ever
     *mentioned* the trigger in a comment. A comment counted as a call site: the same boundary
     error the method note below is about, made while writing up the method note.]

     **Measured**: `renderer=gdi` launches, answers `tacli ui` and writes a `tacli shot` with no
     `vk:` line in its log; `renderer=vulkan`, `openglcore` and `auto` all come up on the Vulkan
     lane in ~210 ms; and a full `scenario load` through the harness's own default path
     (`openglcore` → Vulkan) drives the menus, starts the skirmish, applies 8 of 8 units and
     draws the world at `ss=2` — `vk: world: 2048x1536 target (1024x768 at ss=2)`, 1 501 world
     frames, `census: 6 pass(es) drew (terr=1 feat=1 unit=1 fx=1 mark=1 gui=1)`.

     **Observed and NOT attributed to this landing**: `vk: mark/unit: no hand-over for frame N`
     appears 5 times in a ~2 500-frame run, at frames 17, 927 and 2488 — the first frames, a map
     transition and the last. Nothing here touches the passes that publish those hand-overs, and
     the census shows both drawing on every sampled frame. Named rather than explained.

   * **11-3 — the world passes' GL draw halves.** `tagpu_native.c`, `tagpu_terr.c`,
     `tagpu_feat.c`, `tagpu_fx.c`, `tagpu_shadow.c`, `tagpu_posedraw.c`, `tagpu_hires_draw.c`,
     `tagpu_render3do.c` — ~700 of the 1 233 lines. Every one keeps its producer and its
     `_handover`; only the half below `tagpu_native.c:3753`'s `if (!gl_draws)` goes. Shown by:
     the world A/B at `ss=2` still reads 0 px per pass.

     **SIX OF THE EIGHT ARE DONE AND THE OTHER TWO BREAK THE RULE THIS LINE STATES
     [2026-09-18].** `tagpu_native.c`, `tagpu_terr.c`, `tagpu_feat.c`, `tagpu_fx.c`,
     `tagpu_posedraw.c` and `tagpu_render3do.c` all have the shape the plan assumed: a gather
     that survives, a `_publish`/`_handover` that survives, and a draw half that goes. For
     **`tagpu_shadow.c` and `tagpu_hires_draw.c` the producer IS the half being deleted**, so
     "keeps its producer" cannot be satisfied:

     * `s_pubHave = 1` is set **only** in `tagpu_shadow_end`, and `tagpu_shadow_begin` /
       `_hills` / `_end` were called only from `tagpu_native.c` **below** the `!gl_draws`
       hand-over return (4223, 4310, 4311 against the return at 3817, measured at the 11-2
       landing `22e6e4e`).
     * `s_hiHave = 1` is set **only** in `tagpu_hires_depth`, called at 4287 — below the same
       return — and `tagpu_hires_draw` at 4599 and 4658 likewise.

     **The consequence is not new and is not this landing's doing**: both were already
     unreachable on the Vulkan lane from landing 4b onward, which is why `tagpu_vk_shadow.c:788`
     stands down on `tagpu_shadow_handover` returning 0 — already recorded above for the shadow
     half, and true of the hires half for the same structural reason. Deleting the GL halves
     changes no behaviour. What it changes is the **status** of two Vulkan modules: it makes
     `tagpu_vk_shadow.c` and the caster half of `tagpu_vk_hires.c` unreachable *by construction*
     rather than by an accident nobody has fixed, and it makes the absence of Classic++ soft
     shadows and of replacement-mesh casters on the shipped lane permanent until someone writes
     a Vulkan-side producer.

     **That is a decision about what the program does, not about which lines are dead, so it is
     the owner's** (CLAUDE.md, *Land it yourself*, escalation reason 1). The two readings:

     | | delete the two GL halves now | leave them for a producer landing |
     |---|---|---|
     | behaviour today | unchanged — both are already unreachable | unchanged |
     | what it says | the features are gone until rebuilt on Vulkan | the features are coming back |
     | what follows | `tagpu_vk_shadow.c` and part of `tagpu_vk_hires.c` become deletable too | both stay, waiting for a producer |

     **11-3 lands with the six that fit the rule.** The two are untouched and named here rather
     than decided quietly.
   * **11-4 — the UI and support passes' GL draw halves.** `tagpu_gui_surf.c`, `tagpu_mark.c`,
     `tagpu_text.c`, `tagpu_fps.c`, `tagpu_gaf.c`, `tagpu_posebake.c`, `tagpu_scaffold.c`,
     `tagpu_hires.c`, `tagpu_overlay.c` — the rest of the 1 233. Shown by: the UI A/B, and
     `tacli ui` still answering on every lane.

     **IT IS NOT ONE LANDING AND IT IS NOT ONE SHAPE [surveyed 2026-09-18, after 11-3].**
     Landing 11-3 had one seam per file — a `gl_draws` latch and a draw block under it. This
     one has **65 sites in three different shapes**, and the shapes matter because they cost
     different things to remove:

     | shape | what it is | what removing it costs | count |
     |---|---|---|---|
     | `if (gl_draws) { … }` | 11-3's shape: a draw half inside a function that also gathers | delete the block, drop the latch | 45 |
     | `if (!tagpu_vk_owns_present()) { … }` | a GL block inside a function doing CPU work too | delete the block, keep the function | 10 |
     | `if (tagpu_vk_owns_present()) return;` | a whole function that is now a no-op | delete the function **and audit every caller**, which cascades | 10 |

     They are not spread evenly, which is what makes the split obvious rather than arbitrary:

     * **11-4a — `tagpu_mark.c`** (**25** sites: 23 of the uniform `if (gl_draws)` shape and
       two compound `(textTex || !gl_draws)` conditions — 26 `gl_draws` tokens at `315e496`,
       one of them the declaration. The survey said 24 and the landing's review counted; the
       65-site total above is assembled from these per-file counts, so it is 66). The
       marker layer: health bars, the cursor, the band box, group digits. `tagpu_vk_mark.c` is
       a live twin — the census reports `mark=1` — so the halves are separable the way 11-3's
       were. This is the one to do first, because it is 11-3's shape at a larger scale.

       **DONE [2026-09-18].** 222 lines out, 68 in. `getgl`, `mksh`, `init_gl` and
       `upload_layer` are gone, with the five `x_gl*` entry points, the program/VAO/VBO/texture
       statics and the eight uniform locations; `tagpu_mark_glreset` stays on the context-lost
       cascade purely to forward to `tagpu_text.c`, which still holds a GL object.

       **`s_state` went too, and only because the review pushed back.** The landing first kept
       the readiness flag, reasoning that `glreset` still clears it. That reason does not hold:
       with `init_gl` gone nothing can ever set it to 1, so it was write-only — and no warning
       fires, because `-Wunused-but-set-variable` does not apply to file-scope statics. The
       danger is not the dead `int`, it is the plausible NAME. A later landing re-introducing
       `if (s_state != 1) return;` — the exact guard 11-4a deleted — would return on every
       frame, `mk_push`/`mk_draw` would never run, `s_mkHave` would never reach 1, and
       `tagpu_mark_handover` would return 0 forever: markers silently absent, discovered days
       later from a screenshot. **A flag no code can raise is not state, it is a trap with a
       plausible name**, and every remaining part of 11-4/11-5 will be offered the same trade.
       The gather is untouched: `mk_push` and `mk_draw` still record every bucket and
       `tagpu_mark_handover` still publishes them.

       **Measured — a cross-build frame diff, which this fixture makes nearly exact.**
       `selbox-slope` pins the camera and holds three Stumpys, so the presented frame is
       *static* but for one pixel. Over **8 grabs of one build** the animation set — every
       pixel that differs between any two of them — is exactly **1 px, at (512, 384)**: the
       window centre, where the pointer rests, and TA's own cursor pulses there between
       `[11,11,0]` and `[251,251,251]`. It is drawn by the engine, not by us, and `-draw_mouse 0`
       does not touch it because it is in the frame rather than in X's overlay.

       Excluding that one pixel, **all 16 cross-build pairs** (2 grabs of `315e496` x 8 of this
       landing) differ by **0 px of 786 432** — the whole window, not one pass. Also
       `mark: bars=3` on both builds, `census: 6 pass(es) drew (terr=1 feat=1 unit=1 fx=1
       mark=1 scaf=0 gui=1 fps=0)`, and three green health bars under the three tanks in the
       composited window.

       **The first run of this measurement reported a 0-px noise floor and that was luck**, not
       a floor: two grabs taken seconds apart caught the cursor in the same phase. It only
       surfaced because the post-review rebuild was re-measured and came back **1 px, max
       channel delta 251** — a number large enough to chase rather than round off. Two grabs
       are not a noise floor for anything that pulses; take enough to see the period, and
       report the varying set rather than a single pair's agreement.

       **Not covered by the measurement, and said plainly:** the fixture produced records for
       the BARS bucket only — `cursor=0 ordtri=0 ordline=0 text=0(lab=0)` throughout, on HEAD's
       build as much as on this one, so it is the fixture and not the change. What makes that
       safe to land rather than merely unobserved is that every write to those counters lives
       in `tagpu_mark_emit_line` / `_emit_tri` / `_emit_text`, and `git diff -U0` shows not one
       of those lines in the change set: the buckets are filled upstream of everything removed.

       **The single-pass `.ab` lever could not be used and that is by design**, not a fault:
       `vk: 1 A/B levers claimed this frame and 6 passes drew into it - nothing captured`. A
       Vulkan frame carries every armed pass at once, so a per-pass capture needs the other
       five stood down — which changes the frame it is meant to certify. The full-window
       cross-build diff above answers the same question and is strictly stronger.
     * **11-4b — `tagpu_gui_surf.c`** (30 sites, all three shapes, 9 of the 10 whole-function
       stand-downs). The hard one, and the only part where deletion cascades into callers.

       **SURVEYED 2026-09-19 AND IT IS EASIER THAN THIS ROW SAYS.** The cascade is real but
       **entirely file-local**: all nine stand-downs are `static` and every caller is in
       `tagpu_gui_surf.c` itself — `twin_colour` (3 callers), `twin_col_drop` (2),
       `twin_upload` (2), `twin_clear` (1), `twin_fill` (1), `twin_outline` (1),
       `restore_step` (1), `upload_palette` (1), `unbind_all` (1). Nothing outside the file
       names any of them and no header declares them.

       **One name looks like a cross-file caller and is not**: `tagpu_terr.c:1416` calls a
       `restore_step`, but that is an unrelated file-local static with a different signature
       (`static void restore_step(const char* ta)` at `tagpu_terr.c:1215`, against
       `static void restore_step(void)` at `tagpu_gui_surf.c:1301`). A bare-symbol search — the
       method item 11's survey settled on — reports it as a hit. Read the signature, not the
       name.

       **DONE [2026-09-19].** 796 lines out, 99 in; the file goes 3 179 -> 2 457. The nine
       stand-downs are gone with their call sites, every `if (gl_draws)` and
       `if (!tagpu_vk_owns_present())` block with them, and behind them `init_gl` (68 GL
       calls), `mksh`, `mkprog`, `quad`, `getgl`, `sharp_drop`, the 13 `PFN_*` typedefs, the
       13 `x_gl*` pointers and ~30 `s_u*` uniform locations. **The nine shader strings stay**,
       wrapped in the pragma — they are `tagpu_vk_gui.c`'s shaders, all seven `gui_*` programs
       of the PROGRAMS table.

       **The premise, established rather than assumed.** Every gated site in the file sits in a
       `static`, and a call-graph walk from all nine exports shows **only `tagpu_gui_present`
       reaches any of them**. That one export has a single caller, `tagpu_overlay.c:408` inside
       `tagpu_overlay_draw`, which has a single caller, `render_vk.c:232` — inside the frame
       loop that `render_vk.c:145` opens by calling `tagpu_vk_own_present()`, i.e.
       `InterlockedExchange(&s_ownWin, 1)`, once before the loop. So `gl_draws` is false at all
       30 sites **by an ordering**, not by a hope. `renderer=gdi` reaches `tagpu_overlay_draw`
       never, so it does not reach this file at all.

       **Measured**: `census: … gui=1` on every sampled frame, `tacli ui` still answers
       (`gui ARMMAIN2.GUI 1024x768`), and the cross-build frame diff is **0 px of 786 432 across
       all 64 pairs** (8 grabs x 8 grabs), outside the same one-pixel cursor animation set
       11-4a characterised. The GUI *is* the side panels and the top bar, so a whole-window diff
       is a direct test of this pass rather than a proxy.

       **The review's findings, all acted on.** The pragma bracket above; a dead `float v[24]`
       and an unused `gh` that it had hidden; **`s_clears`**, which the landing had left as a
       counter nothing could increment while `case PK_CLEAR` kept mirroring ops — moved into the
       drain beside its four siblings, and the heartbeat went from a permanent `clears=0` to
       `clears=52351` in one run; and **nineteen write-only statics** (the six programs, VAO/VBO,
       palette texture, the sharp layer's texture and FBO, the minimap's pair, `s_gl`,
       `s_palUpValid`, `s_sharpFailed`), all assigned only `= 0` in `tagpu_gui_glreset` and read
       nowhere. With those gone the file has **no GL dependency at all**, so its
       `opengl_utils.h` include went too — the first of that header's nineteen includers to
       leave it.

       **Four pieces of dead state went with the draw, and one deliberately did not.** Once the
       GL ids stop being created, `twin_sprite` and `twin_copy` return bytes computed from
       `t->rgb`/`src->rgb`/`s_colValid` that can no longer be anything but 0 — 4b-3 had already
       said this lane records "indexed". Those are folded to a literal 0 with the reasoning
       kept, because computing 0 from three flags nothing can raise is landing 11-4a's `s_state`
       trap at record scale: the value is right and the next reader believes the inputs still
       move. `TWIN` loses `tex`, `fbo` and `rgb` (the bookkeeping `surf`/`w`/`h` is what
       `twin_find` and the mirror ops need), and `twin_drop`'s three `glDelete*` calls go with
       them. **`s_colValid` stays**, pinned at 0 and documented on its declaration: unlike
       `s_state` it is not a readiness gate but a VALUE that is published as
       `s_mHand.colourTwins`, and 0 is correct. Removing it would reshape the hand-over, which
       is a protocol change for `tagpu_vk_gui.c` rather than a deletion — out of scope, and
       named here instead of done quietly.

       **And landing 4b-3 already did the hard half.** Each of the nine opens with the
       stand-down as its FIRST statement under a comment saying *"PURE GL: this puts pixels in a
       GL object and feeds nothing the Vulkan twin is told"*, so there is no CPU work to rescue
       from above the gate. `restore_step` carries the one nuance worth keeping: its comment
       records that `s_colValid` stays 0 on this lane, which is what the record's `restored`
       term reads — so the call sites guarded by `if (restored)` are already never taken, and
       deleting them changes nothing. **Its nine shader strings stay** (see the SPIR-V note
       below); `tagpu_gui_surf.c` holds more of them than any other file.
     * **11-4c — `tagpu_fps.c`, `tagpu_gaf.c`, `tagpu_posebake.c`, `tagpu_scaffold.c`**
       (11 sites between them). **Surveyed 2026-09-19: no whole-function stand-downs at all** —
       `tagpu_fps.c` 2 `if (gl_draws)`, `tagpu_gaf.c` 3 plus one `!tagpu_vk_owns_present()`,
       `tagpu_posebake.c` 3 of the latter, `tagpu_scaffold.c` 2 of the former. `tagpu_fps.c` and
       `tagpu_scaffold.c` carry 2 manifest shaders each and must keep them; the other two carry
       none.

       **DONE [2026-09-19].** All four files, 0 lane gates left in any of them.
       `tagpu_fps.c` and `tagpu_scaffold.c` lose `init_gl`, `getgl`, `mksh`, `serr`, the
       `PFN_*`/`x_gl*` blocks and their uniform locations; `tagpu_fps.c` also loses `flog` and
       **`s_state`**, which was 11-4a's trap again — nothing could raise it once `init_gl` went.
       `tagpu_scaffold.c` loses **`tagpu_scaffold_texref`**, an EXPORTED accessor returning the
       pass's GL texture id that had no caller anywhere in the tree even before this landing.
       Both files keep their shader pair behind a pragma whose `pop` was **probed**, not read.

       **`tagpu_gaf.c` is the one that is not like the others, and the plan should say why.**
       Everything else in 11-4 was unreachable; `tagpu_gaf_atlas_create` is **reachable on this
       lane**, from six call sites in five files — one of them `tagpu_gui_surf.c`'s
       `atlas_setup`, which 11-4b deliberately kept. It survives the deletion only because
       landing 4b-2 had already made the GL name optional there: *"THE GL NAME IS OPTIONAL; THE
       LAYOUT IS NOT … the shelf packer, the entry table and the CPU mirror below are the
       atlas, and the twin uploads the mirror."* So the `glGenTextures`/`glTexImage2D` block
       goes and the layout work stays. Same in `atlas_paint`: the `glTexSubImage2D` upload goes
       and **the CPU mirror `memcpy` beside it stays** — it is what `tagpu_vk_*` samples.
       Removing it for the 11-4 reason would have been the right outcome from the wrong
       argument, which stops being right at the next edit.

       **`tagpu_posebake.c` has a site that is NOT a draw gate.**
       `if (!s_mirrorWant && tagpu_vk_owns_present())` is the POSITIVE form: it ARMS the mirror
       for the Vulkan lane. It simplified to `if (!s_mirrorWant)` rather than being deleted.
       Not every `tagpu_vk_owns_present()` is a GL gate, and a sweep that assumes so deletes
       the lane's own bring-up.

       **Measured**: `0 px of 786 432` across all 64 cross-build pairs on the usual fixture,
       atlas healthy (`feat: … atlas=24`, `fx: … atlas=10` — a broken GAF mirror would show as
       missing sprites everywhere). Then, because the default fixture leaves `scaf=0 fps=0`,
       both were armed: the census goes to `7 pass(es) drew … scaf=1 … fps=1` and a window grab
       shows `FPS59` top-left and the scaffold's blue→red ramp on the blocked cells, which is
       its fragment shader's exact gradient.

       **Observed, and now ATTRIBUTED — to something older than this landing.** With the
       scaffold armed the census also reports `unit=0`, and the tank bodies are absent from
       that grab while their health bars remain; disarming it returns `unit=1`. The scaffold is
       an occlusion input to the unit shader (`tagpu_native.c:508`, *"scaffold occlusion:
       nearer stamped rows hide this fragment"*), and `tagpu_native.c` is untouched by 11-4c.

       The landing's own review closed the rest, **by construction rather than by one more
       run**. Every construct 11-4c removed from `tagpu_scaffold_frame` sits inside
       `if (gl_draws)`, or is `if (gl_draws && s_state == 2) return;`, or is the
       `const int gl_draws = !tagpu_vk_owns_present();` declaration itself — and `gl_draws` is
       0 at this site by the ordering at the top of this plan. Under `gl_draws == 0` deleting a
       never-entered block and a never-taken early return is an EXACT transformation, not a
       nearly-exact one. Everything else in the file is byte-identical to the pre-landing
       version: `tagpu_scaffold_overlay`, `tagpu_scaffold_frameinfo`, `stamp_gaf` and
       `stamp_px` were extracted from both revisions with a masked brace matcher and hash to
       the same value, and `tagpu_scaffold_frame`'s gather, its `s_pubBuf`/`s_pubW`/`s_pubH`/
       `s_pubRect`/`s_pubRows` publication and its A/B capture are unchanged. **So `unit=0`
       with the scaffold armed reproduces at the pre-landing tree, and 11-4c cannot be its
       cause.** What it actually is remains open and belongs to whoever next arms the scaffold.

       **What the 64 pairs did and did not cover, said plainly.** They were taken on the usual
       fixture, which leaves `scaf=0 fps=0` — so the two passes this landing changed most
       contributed no pixels to the only pixel-exact measurement in it. The armed run was
       checked by eye (`FPS59`, the ramp) and not by diff. A regression confined to the armed
       path would have passed every gate here; the argument above is what stands in for that,
       and it is an argument about the shape of the diff rather than about a picture.

       **THE REVIEW'S OTHER FINDING IS THE ONE WORTH CARRYING FORWARD: a uniform sweep can
       remove a trap from one file and leave the identical trap in the next.** 11-4c deleted
       `tagpu_fps.c`'s `s_state` for the exact reason the taxonomy gives — dead state that
       feeds a GUARD is a trap, because nothing could raise it once the bring-up went — and in
       the same landing left `tagpu_scaffold.c`'s `s_state`, plus `s_texW`/`s_texH`, write-only
       in precisely the same way. Two surviving comments still presented it as live state, one
       of them inside `tagpu_scaffold_frameinfo`, **the function the 4b-1 review had already
       fixed once for testing it**. The failure that was one edit away: a later reader sees the
       static still declared and still assigned, reads it as live state that merely happens to
       be 0, and re-adds the readiness guard; `s_state` can never reach 1, so `frameinfo`
       returns 0 forever and the unit shader's occlusion input silently loses its rows. All
       three statics are gone and the comments now say there is no state left to ask.

       The same sweep left `tagpu_posebake.c`'s last three GL calls —
       `glDeleteVertexArrays` and two `glDeleteBuffers`, each guarded by a `vao`/`vbo` whose
       only writer was the bring-up the landing deleted, hence permanently false. Removed;
       `tagpu_posebake_glreset` is the generation bump that was always the API-independent part
       of it, and the file now has no GL call, type, constant or include. The FIELDS stay,
       because `tagpu_posedraw.c` still reads `m->vao` — and its own guard,
       `if (!tagpu_vk_owns_present() && !m->vao) return NULL;`, now has BOTH terms dead. That
       one is 11-5's, and it is exactly the kind of thing 11-5 has to look for rather than
       inherit.

       **And the finding with the real downside was about prose.** Deleting the GL half orphans
       the comments that explain the CPU mirror — "the very rows the line above hands GL", with
       no line above and nothing sampling `tex` any more. Read today they assert a property of
       nothing, which makes the mirror `memcpy` look like redundancy to remove; it is the only
       copy the art has. Each now states what the mirror IS rather than what it mirrored, and
       says outright that deleting it deletes the sprite. **The general rule for the rest of
       this plan: when a landing deletes one half of a pair, the surviving half's comment is
       part of the diff, not documentation debt** — it is what stops the next landing deleting
       the half that is load-bearing.

       **RE-VERIFIED AFTER THE FIXES BY OBJECT IDENTITY, WHICH IS STRONGER HERE THAN ANOTHER
       A/B.** The landing discipline asks for a re-measurement when the code moves, and the
       code did. But a pixel A/B samples a few frames of a few fixtures, whereas comparing the
       EMITTED OBJECT is exhaustive: if the compiler produces the same bytes, no input can
       produce a different pixel. Each file was compiled from both revisions at identical
       paths (the path matters — `__FILE__`-shaped differences perturb the object on their
       own, which is what a first attempt at this measured instead of the change):

       * `tagpu_scaffold.o`, `tagpu_gaf.o`, `tagpu_fps.o` — **byte-identical**. Deleting three
         write-only statics, writing `a->tex = 0` where `a->tex = t` stood with `t` a
         never-reassigned zero, and every comment rewrite, emitted the same machine code.
       * `tagpu_posebake.o` — differs, and only as intended. The object's external references
         lose exactly `_glDeleteBuffers` and `_glDeleteVertexArrays` and **gain nothing**; the
         defined-symbol set is identical; `tagpu_posebake_glreset` goes from 33 instructions to
         3, which is the generation bump and the return. `tagpu_posebake_frame` grew by 31
         instructions while referencing no symbol it did not reference before — GCC re-laying
         out an inline decision once the translation unit shrank, which cannot introduce a call
         that was not already there.
       * The `#include "opengl_utils.h"` removal was checked on its own: the same source with
         the include restored compiles to a **byte-identical** object, so the removal is inert
         rather than merely harmless-looking. An include is the one edit in this set that could
         have changed a macro silently, so it is the one worth proving separately.

     **`tagpu_text.c` and `tagpu_hires.c` are NOT in 11-4 after all.** They have zero lane
     gates, which is the shape that made `tagpu_shadow.c` and `tagpu_hires_draw.c` an
     escalation — but checked directly, they are the opposite case: every entry point still has
     live external callers, because both are CPU-side **producers** (the glyph feed, the mesh
     loader) that both lanes consume. Only their GL-object accessors (`_tex`, `_atlas`, `_vao`,
     `_glreset`) are dead, and those belong to 11-5 with the rest of the GL surface.

     **And a set nobody has scoped yet:** with the draw halves gone, each world pass still
     carries its GLSL source strings and its `init_gl` — **~800 lines across `tagpu_native.c`,
     `tagpu_terr.c`, `tagpu_feat.c`, `tagpu_fx.c` and `tagpu_posedraw.c`**. The `init_gl` half
     of that IS unreachable, because `init_gl` is called only inside
     `if (!tagpu_vk_owns_present())`, and it is neither a draw half (11-3) nor the shared GL
     entry-point surface as 11-5 words it; its home is 11-5.

     **THE GLSL STRINGS ARE NOT IN THAT SET AND MUST NEVER BE DELETED [corrected 2026-09-18,
     by 11-4a's build gate].** This paragraph said "all unreachable" and that half of it was
     wrong. `VS` and `FS` in each pass are the **source of truth for the Vulkan shaders**:
     `tools/spirv-gen.py` reads them out of the PREPROCESSED translation unit, translates them,
     and commits the result as `inc/spirv/<pass>.spv.h`, which is what `tagpu_vk_<pass>.c`
     `#include`s and draws with. They are unreferenced *by C code* and live *as a build input*,
     which is a distinction no grep for callers can see. 11-4a deleted `tagpu_mark`'s pair
     along with `mksh` and `init_gl` and the build stopped at once:

     ```
     spirv-gen: the manifest names tagpu_mark::VS and the source does not have it
     ```

     That gate — `tools/spirv-check.sh`, an order-only prerequisite of the link — is what stands
     between "delete the unreachable GL apparatus" and silently retargeting every ported pass's
     shader. **Bound it correctly, though:** it compares `sha256` of the **post-transform**
     Vulkan GLSL (`spirv-gen.py:1166`), not of the C string, so it catches a deletion or any
     edit that changes the translated shader — it does not promise to catch an edit that
     `transform` normalises away. For a DELETION, which is this plan's whole failure mode, it is
     exact. Do not lean on it as a general "the string cannot change" guarantee; where a landing
     restores a shader, diff it against the previous commit as well. Note what it means for the *shape* of 11-5: a file can be a
     GL-free pass and still have to keep two `static const char*` at file scope forever.

     **PROVE THE BRACKET; READING IT IS NOT ENOUGH [added 2026-09-19 after 11-4b].** Landing
     11-4b put its `pop` **inside a `/* … */` block**, where it is comment text and not a
     directive, so `-Wunused-variable` stayed off for the rest of the file and hid two of that
     landing's own leftovers. The first fix repeated the bug — a regex for the last shader's
     terminating `;` stopped at a `;` ending a line *inside that same comment*. Find the
     terminator with comments and strings MASKED, then plant an unused static immediately after
     the `pop` and confirm the compiler reports it. Both broken placements read correctly; only
     the probe distinguishes them. (11-4a's bracket was probed afterwards and is sound.)

     **The idiom that leaves, written once here because every remaining part will need it.**
     With no GL consumer the strings warn as `-Wunused-variable`. `__attribute__((unused))` is
     the obvious silencer and it is **position-sensitive**, which is the part to get right —
     `_DECL` is applied with `re.search` on the stripped line (`spirv-gen.py:532`), so it
     tolerates a prefix but not an infix:

     | placement | extracted? |
     |---|---|
     | `static const char* VS = …` | yes |
     | `__attribute__((unused)) static const char* VS = …` | **yes** |
     | `static const char* VS __attribute__((unused)) = …` | no |
     | `static const char* __attribute__((unused)) VS = …` | no |

     [Corrected 2026-09-19 by 11-4a's review, which ran the regex rather than reading it: this
     paragraph first said the attribute could not work at all, and two of the four placements
     do.] A wrong placement fails LOUDLY — the manifest reports the shader missing — so it
     cannot ship silently either way. The pragma below is still what 11-4a used, chosen because
     it brackets the pair visibly and carries the comment explaining why two unreferenced
     strings are in the file at all; the prefix attribute is equally correct if a file's layout
     suits it better. What must NOT happen is reaching for the wide tool — a `#pragma` with no
     `pop`, or `-Wno-unused-variable` in `CFLAGS` — which would stop that file, or the build,
     reporting genuinely orphaned statics:

     ```c
     #pragma GCC diagnostic push
     #pragma GCC diagnostic ignored "-Wunused-variable"
     static const char* VS = ...
     static const char* FS = ...
     #pragma GCC diagnostic pop
     ```
   * **THE RULE THIS GATE FOUND, and it is the transferable part: deleting a backend is not
     mostly about deleting calls. It is about finding the PREDICATES that quietly encode "the
     backend is ready" as "the work is possible."** A GL object name — `s_state`, a texture, a
     VAO — reads like a fact about our own progress, so it gets used as a gate on work that has
     nothing to do with GL. On a lane with no GL context that name is permanently 0, and every
     gate keyed on it is permanently shut. **The compiler cannot see this**: the state is
     written, read and consistent; only its VALUE is pinned. Nor can a pixel A/B, because the
     failure is something that does not happen.

     Four consecutive landings, four instances, and they get worse as they go:

     1. **11-4c** — `tagpu_scaffold_frameinfo` published its answer only when `s_state` said
        the GL program was ready.
     2. **11-5b** — `tagpu_owndraw_set_structshadow(s_armed == 1 && gl_draws && suppress, …)`.
        Would have pinned the structure-shadow gate at 0 for any FUTURE painter.
     3. **11-5c** — `tagpu_terr_hills_draw`'s `!s_hVao`, sitting above the publication of the
        CPU caster mesh the Vulkan shadow pass exists to receive. (A pin removed on a route
        that has ANOTHER pin in front of it: `tagpu_shadow_hills` is called from nowhere and
        would return at `!s_live` anyway. Worth removing, but the route is not open and the
        first version of this line read as though it were.)
     4. **11-5c** — `restore_step`'s `!s_atlasTex`, which made the ONLY publisher of the restore
        frame list unreachable, and `restored`'s `s_rgbState`, which pinned the consumer's
        `uRestored` at 0. Between them, `tagpu_restorevk.on` armed a fully implemented consumer
        that was never sent anything and would have been told not to sample it if it had been.

     **How to find them, since reading for "GL calls" does not.** For every GL name a file
     defines, ask what tests it, and for each test ask whether the thing behind it is GL. A
     mesh, a mirror, a serial, a published struct field and a device limit are not. Three of
     the four above were found this way and one (11-5b's) by the landing review, which is the
     rate to expect. The positive form is the fix in every case: gate on the thing itself —
     `s_atlasBuilt`, `s_hMeshW`, `s_rFrames`, `s_ncell` — not on the name of the object some
     backend built out of it.

     **AND THE COROLLARY, WHICH 11-5c PAID FOR: unpinning a predicate runs every consumer of
     it for the first time, and a consumer written for two lanes may hold a rule that only
     made sense with a twin.** 11-5c made `restored` honest and thereby fired
     `tagpu_vk_terr.c`'s parity refusal — "the twin is drawing restored and we have not, so
     draw NOTHING" — which with no twin, and with the restorer's refusal latched one-way for
     the device's life, meant **no terrain at all for the session** on any device that
     declined the restorer. It was caught by the landing review, not by the landing.
     So: after removing a pin, **go and read every reader of the predicate**, not just the
     producer. The question to ask each one is "what did this do when the other lane existed,
     and is that still what it should do?" Parity rules are the ones to suspect: they are
     written as refusals, refusals are silent, and a refusal that used to be unreachable has
     never been seen to fire.

     **AND THE SECOND COROLLARY, WHICH 11-5d PAID FOR: not every pinned predicate is an
     accident, and the ones that are deliberate hide their cost in the JUSTIFICATION rather
     than in the value.** `tagpu_posedraw_live()` looks exactly like the four above —
     `s_state == 1 && !tagpu_vk_owns_present()`, a lane term pinned false — but the 4b-2
     landing review wrote it that way on purpose, with an argument: this pass hands over rather
     than draws, so it cannot promise the consumer ran. That argument is still sound. What was
     NOT sound was the sentence attached to it to make the cost acceptable — *"the engine
     keeps its own rasterise"*, which every later reader, and `tagpu_owndraw.c`'s own
     paragraph, took to mean "so a unit we fail to draw is still on screen in 8bpp". Measured:
     it is not on screen at all. **So when a pin turns out to be deliberate, do not stop at
     "the value is correct" — go and measure the sentence that made the value acceptable.**
     A pin that is wrong shows up as a bug; a justification that is wrong shows up as a design
     everyone trusts, and it will not be the compiler that finds it either.

     **AND THE THIRD COROLLARY, WHICH 11-5e-1 PAID FOR: a predicate can be pinned by the
     ABSENCE of a thing rather than by a value, and when it is, the root of a dead TREE is
     that predicate and not any function in the tree.** `tagpu_overlay.c` polled
     `wglGetCurrentContext` once a frame and ran six modules' `*_glreset` when the handle
     changed. Nothing about that code says "GL is gone": every variable is live, every branch
     is ordinary, and its own caller runs every frame. It is dead because **`oglu_load_dll()`,
     the only code that resolves `wglCreateContext` and `wglMakeCurrent`, has no caller** --
     a fact about what is NOT in the tree, which no amount of reading that file can establish.
     Deleting the watch orphaned sixteen functions at once, **across sixteen files — one
     `*_glreset` per file** — none of which a caller scan had flagged, because each of them did
     have a caller: the one above it.

     **AND THE FIRST STATEMENT OF THIS COROLLARY GOT ITS OWN INVARIANT WRONG, which is worth
     more than the corollary.** It said the names `wglCreateContext` and `wglMakeCurrent`
     "appear in no source of this build". They appear in `opengl_utils.c`, as the strings a
     `GetProcAddress` resolves into `xwgl*` pointers; a word-bounded scan missed them. A name
     scan is not an absence proof -- it is a proof about spelling, and it stays true while a
     future landing calls `oglu_load_dll()` and restores the failure the watch existed for.
     **When a predicate is pinned by an absence, the invariant to write down is the CALLER
     that is missing, never the name.** [The 11-5e-1 review's MEDIUM-1.]

     **So the search is not "which functions have no callers" -- that finds leaves. It is
     "which tests can never be true", and what falls out is everything reachable only through
     them.** The two searches answer different questions and only the second finds a cascade.
     Corollary 1 is its mirror image: unpinning a predicate that is still LIVE runs its
     consumers for the first time, and unpinning one that is DEAD orphans them. Establish
     which kind you have before you touch it -- by scanning the whole tree for the thing whose
     absence pins it, not by reading the file the predicate is in.

   * **11-5 — what is left of the GL entry-point surface. FIVE PARTS, and the split follows
     the passes rather than a calendar** (written 2026-09-19, by the landing that measured it;
     `M` moved from four to five when 11-5c re-measured the surface and `tagpu_posedraw.c`'s
     122 sites turned out to need their own reachability argument, exactly as `tagpu_terr.c`'s
     did — a count that grew is the plan catching up with the work). The row was filed as one
     thing because it reads as "delete some headers"; the work showed the headers cannot go
     until their callers do, and the callers are passes.

     - **11-5a — the feature and effects passes' GL bring-up. LANDED.** `tagpu_feat.c` and
       `tagpu_fx.c`, 98 GL call sites between them, all of it in `init_gl`/`mksh` because 11-3
       had already taken both draw halves and neither file had an `x_gl*` CALL SITE left. Both
       are now GL-free — no call, no type, no constant, no `opengl_utils.h` — and both gates
       are rewritten into the positive `if (!s_atlas.made) { atlas_setup(); … }` form.
     - **11-5b — `tagpu_native.c`. LANDED**, and it earned its separate row. 139 GL call
       sites: 112 in `init_gl`, 15 in `tagpu_native_frame`'s three `if (gl_draws)` blocks, 7 in
       `tex2d`, 5 in `mksh`; 367 lines out and the file is GL-free. **The thing it found was
       not a GL call but a GATE: `tagpu_owndraw_set_structshadow(s_armed == 1 && gl_draws &&
       suppress, …)`.** The `gl_draws` term guarded a lane switch that no longer exists, and
       leaving it would have pinned the gate at 0 for any future painter — the engine drawing
       its slant shadows underneath ours, with nothing to report it. It also exposed that
       `s_ssSuppress` has had no writer since 11-3 took the GL draw half (verified across
       `31c700d` and `3771ec4`), so the gate publishes 0 every frame today; a Vulkan-side
       painter must set it. See gpu-status §2.70.
     - **11-5c — `tagpu_terr.c`. LANDED.** 107 sites spread over nine functions — `init_gl`
       (53), `build_hills` (12), `build_height` (10), `ensure_atlas` (10), `glsl_begin` (9),
       `mksh` (5), `dump_if_armed` (4), `tagpu_terr_hills_draw` (3), `rgb_mirror_step` (1). The
       spread is why it was separate from 11-5b: each one needed its own reachability argument
       rather than one shared gate. 541 lines out, 159 in, 31 symbols out of the object and one
       in. **What it found is instances 3 and 4 of the rule above**, and the second of them is
       the largest thing this gate has turned up: `glsl_begin` was doing two jobs, and the
       API-independent one — the centre-out restore order and the per-tile frame list — was the
       ONLY writer of `s_rFrames`, the hand-over `tagpu_restorevk.on` exists to fill. It sat
       below `restore_step`'s `!s_atlasTex`. So the lever armed `tagpu_vk_terr.c`'s restore job,
       its atlas image and its painter, and fed them nothing — and `restored`, the flag that
       sets `uRestored` in that consumer's shader, was computed from the GL restorer's own state
       machine and could only publish 0. The order and the list are now `restore_publish`, with
       no GL and no local job; `restored` reads `s_rFrames`. Default behaviour is unchanged and
       provably so: without the lever, `restore_step` returns on its first line.
       **MEASURED BY RUNNING IT, 2026-09-19** (Two Continents, `renderer=vulkan`,
       `restorevk.on`): `terr: restore request published -- 5062 frames` — a line no build
       before this one could print — then the consumer's `job started`, `restoring the tile
       atlas HERE`, and `restored atlas painted here - 5062 frames, no mirror and no
       read-back`, with `terr=1` on every census after the map loads. The route works end to
       end for the first time since 4b-2. See gpu-status §2.71.
     - **11-5d — `tagpu_posedraw.c`.** 122 sites, and its own row for the same reason
       `tagpu_terr.c` had one: the reachability argument is per function, not per file, and the
       file is 1400+ lines. **Ten of its eleven GL-bearing entry points have NO CALLER anywhere
       in the tree** (`wire_begin`, `depth_begin`, `shadow_set`, `slant_set`, `wire_unit`,
       `slant_begin`, `shadow_begin`, `depth_unit`, `redraw`, `slant_redraw` — 62 of the 122
       sites). The eleventh, `tagpu_posedraw_ready`, is live: `tagpu_native.c:3188` reads it as
       `pdReady`, and it takes a Vulkan arm at `:542` that asks `tagpu_vk_max_uniform_range()`
       and returns 1 before the 48-call GL bring-up below it. **Do not assume from that that
       the file is dead** — `:1032` is `if (!tagpu_vk_owns_present() && !m->vao) return NULL;`,
       a live path with a GL name in it, and `:1420` carries the same `s_mirrorWant` latch
       terrain has.

       **AND IT ALREADY HAS ITS INSTANCE OF THE RULE, MEASURED 2026-09-19 — this one is on the
       SHIPPED lane and it is not a trap for a future producer, it is live today.**
       `tagpu_posedraw_live()` was `s_state == 1 && !tagpu_vk_owns_present()` (`:162` as this
       paragraph was written; `return 0;` at `:202` since the landing), so it is
       **0 on this lane by construction** — while `s_state` is genuinely 1, set by that same
       Vulkan arm, which logged `posedraw: armed for the Vulkan lane — no GL program` and now
       logs `posedraw: armed — no rasteriser of its own, …`. Neither old string exists in the
       tree any more, so a grep for one finds only this note. Landing
       11-3 met the predicate in `tagpu_native.c` and removed a dependent on it ("that
       predicate is false on this lane by definition", `:1996`); it did not sweep the other two
       readers, both in `tagpu_owndraw.c`:

       * `:418` — `tagpu_owndraw_classify` returns 0 ("do not skip") for every unit
         `target_covers()` matches, so **the engine's own 8bpp unit rasterise is never
         skipped**, and the `tagpu_r3dcache_wipe`/`_restore` branch under it never runs.
       * `:839` — `tagpu_owndraw_preshadow` returns before emptying the engine's unit-shadow
         composite.

       **It is silent**: the warning at `:421` is gated on `tagpu_posedraw_refused()`, which is
       `s_state == 2` (`:167`) and therefore false.

       Measured by running it (Two Continents, `renderer=vulkan`, `native.on=all`):
       `OWND target=all skipped=0 passed=1062` — and 1003, and 1075 — in consecutive windows
       while a commander walked, with **`skipped=0` throughout and a session total of
       `skipped=0 passed=1954+`**. An engine-surface capture in those frames shows the
       commander drawn at 8bpp with its health bar and shadow. That surface is presented: the
       Vulkan world is drawn into an offscreen target cleared to TRANSPARENT and "the composite
       blends it over TA's frame" (`tagpu_vk_world.c:317`, `:540`). So the engine's unit is on
       screen underneath ours, every frame, for the session — the "near-invisible 8bpp-under-RGB
       double draw" that `tagpu_owndraw.c:414` describes as the transient case.

       **LANDED 2026-09-19, AND THE PARAGRAPH ABOVE IS PART OF WHAT IT DISPROVED**
       ([gpu-status](gpu-status.html) §2.72). All 122 sites out, 483 lines out and 122 in;
       fourteen functions deleted, ten of them already callerless. `tagpu_posedraw_ready` is in
       the rule's positive form — one `maxUniformBufferRange` compare against `PD_BLOCK`, no
       lane test in front of it. `VS` and `DFS` stay behind a probed pragma; they are
       `spirv-gen.py:186-187`'s source for the `pose_unit` and `pose_depth` pipelines.

       **WHAT THE MEASUREMENT FOUND — in two rounds, because the first round's conclusion was
       wrong and the landing review caught it.** The question was *"whether our fragments cover
       the engine's opaquely everywhere"*. `one-unit` on Two Continents, 1024×768, one ARMCOM
       at screen (512,384):

       * `OWND target=all skipped=0 passed=55991` — the engine really does rasterise every
         unit, every frame, which is this predicate's doing; on gdi the detours are armed too
         and `live()` is a literal `return 0`, so `classify` cannot skip on either lane;
       * `tacli shot` (TA's own surface) carries that commander in colour with its drop shadow,
         in every configuration tried on both lanes;
       * `renderer=vulkan`, `native.on` off so nothing of ours draws a unit: the commander is
         **absent from the presented frame** — our marker cross and health bar hang over empty
         ground. With `terr.on` off as well, TA's own terrain **does** reach the frame (it comes
         up green) and the commander is **still absent**, so our terrain is not what hides it.
       * **`renderer=gdi`, same fixture, same two levers off: the commander IS THERE.** The
         window capture and the engine-surface capture are the same picture.

       **ROUND 1 CONCLUDED "the engine's per-unit rasterise is invisible work" AND THAT IS
       FALSE.** The review asked for the gdi control precisely because the first three readings
       localise the loss to our own pipeline just as well as to the engine, and it is one
       relaunch. The same engine output reaches the player perfectly well one lane over, so
       **the loss is OURS and it is in the Vulkan composite path** — not a property of the
       engine, and fixable.

       **What that does to the 4b-2 comfort.** *"The engine keeps its own rasterise"*, read as
       "so a unit we fail to draw is still on screen in 8bpp", is **TRUE on gdi and FALSE on
       Vulkan**, and false because of a defect of ours rather than a wrong design. So
       `tagpu_posedraw_live()` returning 0 is not merely safe, it is **load-bearing and more so
       than 4b-2 knew**: while the composite drops TA's units, a `live()` of 1 would take the
       engine's copy away as well and a stood-down frame would have nothing on it at all. Once
       the composite is fixed the fallback is real, as gdi already shows.

       **THE FIX SHAPE PROPOSED ABOVE IS WITHDRAWN.** `tagpu_posedraw_live()` is `return 0` with
       the measurement written at it — value-identical to what it had always returned, so no
       consumer runs for the first time.

       **AND THE MECHANISM IS A LEAD, NOT A FINDING.** `tagpu_surf_take` (`tagpu_surf.c:32`, from
       `tagpu_overlay_draw`) copies `g_ddraw.primary->surface` on the RENDER thread;
       `ss_shot_service` reads the same object at the entry of the engine's flip on the GAME
       thread, where its comment says it holds *"the frame the PREVIOUS flip presented"*.
       `tagpu_surf.h`'s argument is explicitly a LIFETIME one and says nothing about the buffer
       holding a FINISHED frame, and TA writes those bytes without entering `g_ddraw.cs`. Settle
       it by dumping the snapshot's bytes in the frame `tacli shot` fires and diffing; if it
       holds, the fix is an ORDERING and never a timing mitigation. **Not 11-5d's to make** —
       a cross-thread change belongs in a landing whose review is briefed on it. `preshadow`
       still wants its own question when that day comes.
     - **11-5e — the entry-point surface this row names. THREE LANDINGS, and the split was
       forced by a dependency the row had backwards** (written 2026-09-19 by 11-5e-1, which
       found it). The row said this part *ends at* `opengl_utils.c`. It cannot contain that
       file at all: **`opengl_utils.c` DEFINES the GL entry points every other GL file calls**
       — `glBindTexture`, `glTexImage2D`, `xwglGetProcAddress`, about fifty names used by
       `tagpu_restoreglsl.c`, `tagpu_shadow.c`, `tagpu_hires*.c`, `tagpu_gaf.c` — so it goes
       last of everything, and `tagpu_shadow.c` and `tagpu_hires_draw.c` stand in front of it
       behind **escalation reason 1**. 11-5e therefore ends one file short of where the row
       said it would, and that last step belongs to whatever follows the escalation.

       - **11-5e-1 — the three callerless leaf files. LANDED 2026-09-19.** `tagpu_ftime.c`
         (10 sites), `tagpu_text.c` (20), `tagpu_overlay.c` (22): **52 sites, three files
         GL-free, thirteen functions deleted**, every one callerless under a masked-comment
         scan and every one confirmed gone from its object file's symbol table with nothing
         else taken with it. Its real output is the third corollary above and the cascade it
         found; the deletions are the smaller half. Verified by running it: the presented
         frame is **pixel-identical to the pre-landing build over the whole frame except the
         minimap**, where 44 pixels of unit blips differ because the two runs' AI had built
         different numbers of off-screen units (`alive=14` against `alive=9`, `onscreen=1`
         in both).
       - **11-5e-2 — `tagpu_gaf.c` (39) and `tagpu_restoreglsl.c` (249). LANDED 2026-09-19.**
         `tagpu_restoreglsl.c` deleted entire (39 functions), `tagpu_gaf.c` down to zero GL
         sites, the surface **540 → 252** and the GL-bearing files **six → four**. 45 functions
         gone (47 with the two the review's fix removed), inventory diffed per TU, every
         deleted non-static absent from every object.
         Measured (re-run after the review's fix, three full runs on a fresh instance):
         within a run **0 px** over nine pairs; the same build across two RUNS **44 px**, all
         in the minimap — that is the noise floor; and `main` vs this build **44 px of
         786 432, with ZERO outside the minimap rect**. The cross-build figure IS the noise
         floor, to the pixel. The direct evidence is the log rather than the pixels, the claim
         being behaviour-preservation: `fx`/`feat` `0 entries seeded`, `unit` `25 entries
         seeded`, `vk: unit: restoring the twin HERE - 25 of 25 frames` — byte-identical
         between the two DLLs.

         **The root predicate was `a->tex`, and landing 11-4c set it to 0 on purpose hours
         earlier.** `tagpu_rglsl_job_new`'s one call site sits below `!a->tex` in
         `tagpu_gaf_atlas_restore`, so no job was ever made, `gl_ready()` never turned on, and
         `tagpu_rcore_step` returned on its second line every frame. **That is corollary 2, not
         corollary 3** — a value set deliberately, whose cost hides in its justification:
         11-4c argued about stale texture names and did not notice it had also put 288 GL call
         sites and one live feed out of reach. The missing `LoadLibrary` is a second pin
         underneath and would stop the same tree alone, but it is not the root.

         **AND UNPINNING IT RAN A CONSUMER THAT HAD NEVER RUN.** `atlas_paint` ended in
         `if (a->job) restore_enqueue(a, e);`, and `restore_enqueue` feeds the GL job **and**
         the PUBLISHED FRAME LIST — the Vulkan lane's restore input. So a GL predicate was
         gating the other backend's feed, shut since 11-4c: the list was seeded once by
         `tagpu_gaf_atlas_restore_vk`, emptied by the first `job_clear_dest` (a recycle or a
         repack), and never refilled, because the only re-seed is reachable from the arm and
         from an overflow inside the call that never ran. Probed in-process, pre-landing:
         `PAINT AFTER ARM n=15 job=NULL rlistN=0 (this paint will NOT enqueue)` and
         `restoring the atlas HERE - 0 of 0 frames`, 0 queue drains. Post-landing:
         `rlistN 0 → 1`, `feat 15 of 15`, `fx 5 of 5`, 162 drains. **No pixel A/B could have
         caught it**: the feed exists only under `tagpu_restorevk.on`, which 11-4c's fixture
         did not arm. 11-5e-1's HIGH generalised — *a pixel diff cannot cover a lever the
         fixture does not arm*.

         **BUT THE ONE-LINE FIX WAS A USE-AFTER-FREE AND IS NOT IN THIS LANDING.** The second
         reviewer caught it and every link was then checked against the source:
         `tagpu_posedraw.c`'s `pd_view_publish` captures `s_atlas.rlist` as a RAW pointer on
         the FIRST posedraw window of the frame, and `tagpu_native.c`'s `ghost_record` runs
         after it — deliberately, its comment says so — and can reach `atlas_paint` through
         `tagpu_r3d_atlas_uv` → `atlas_get` on a build ghost whose texture is not yet atlased.
         `rlist_add` → `rlist_room` then `realloc`s the buffer the render thread is about to
         read. Feature and effects escape only because their publication happens to be the
         last write of their frame; the unit atlas does not, and nothing enforces it. So the
         call stays out, which is **exactly today's behaviour** (`a->job` was always NULL) and
         keeps the deletion behaviour-preserving, and **`restore_enqueue` and `rlist_add` are
         deleted with it** — which leaves the property STRONGER than it found it: `rlist_room`
         is now reachable only from `rlist_restart`, and that only from the one-shot arm, so
         `a->rlist` is assigned once and never moved or freed. A lifetime the code enforces,
         where before it rested on `a->job` being NULL for a reason stated three files away.
       - **AND IT INHERITS ONE MORE THING, from 11-5e-2b part 2's review (F4).**
         `tagpu_gaf_atlas_free_buffers` frees `mirror` and not `rlist`, and that is safe only
         because its single caller is `tagpu_gui_surf.c`'s `atlas_setup` — the **GUI** atlas,
         whose `rlist` is NULL for the life of the process because the GUI never arms a list.
         `atlas_setup` then memsets the struct. So the day the UI is given a published list, that
         memset drops a live pointer on every re-arm. Whoever arms it owns the fix: free `rlist`
         and its three fields in `free_buffers`, or give `atlas_setup` a path that does not zero
         over it. Not fixed in part 2 on purpose — `rlist`'s lifetime is this landing's subject,
         and a half-fix (free the buffer, leave `rlistWant`/`rlistFailed` latched) is the exact
         shape of bug this gate keeps finding.
       - **11-5e-2c — restore the feed, safely. LANDED 2026-09-19.** The defect above, fixed by construction
         rather than by a guard, in a landing that can measure it: a **bound** (allocate
         `rlist_cap(a)` once in `tagpu_gaf_atlas_restore_vk`, make `rlist_room` a pure bounds
         test — 0.36 MB unit, 0.72 MB feat, against a mirror this file already declines to
         free at 16 MB), an **ordering** for `rlist_restart`, which rewrites in place and so is
         not covered by the bound (take the unit list in `tagpu_posedraw_handover`, after every
         paint of the frame), and then the two pins that would otherwise make it invisible or
         fatal: **`restored`**, published by all three of `tagpu_feat.c`, `tagpu_fx.c` and
         `tagpu_posedraw.c` and gated in each on `s_atlas.rgb`, whose only remaining write is
         `= 0`; and **`rgbAniso`**, which lost its writer here and reports 0 while
         `tagpu_vk_unit.c` stands the frame down on `h.atlasRgbAniso != s_twinAniso`. Unpin
         the feed without those and the unit pass draws nothing. All three are labelled
         `[PINNED]` at their declarations in `tagpu_gaf.h`, and `atlas_paint` carries the whole
         argument at the site.

         **THIS ROW SAID `s_twinAniso` DEFAULTS TO 4. IT DOES NOT, AND THE LANDING REVERSED THE
         PRESCRIPTION BECAUSE OF IT.** `s_twinAniso` is the knob **clamped by the device**:
         `tagpu_vk_unit.c` sets it to `0.0f` unless the anisotropy extension is present *and*
         `d->maxAniso >= want`. So publishing the documented constant `TAGPU_GAF_TWIN_ANISO`
         would have stood the unit pass down on every machine without anisotropic filtering —
         this row's own predicted failure, arriving from the other side. The producer publishes
         the **knob**; the consumer compares against `s_twinAnisoWant`, the unclamped value it
         read. Both prescriptions in `tagpu_gaf.h` are corrected in place rather than left
         standing.

         **AND THE FLAG REACHES THE SHADER, so "invisible" was literal.** `restored` is copied
         into the unit uniform block at `tagpu_vk_unit.c:1696` and read by the fragment shader
         as `uRestored` (`tagpu_native.c:442`), which does
         `if (uRestored == 1) t = texture(uAtlasRGB, vUV);` at `:470`. At 0 the sample never
         happens and the fragment takes the palette path — so every session since 11-5e-2 built,
         painted, mipped and **bound** a restored twin that no shader read. Measured: an
         interleaved four-run A/B against `515714c` moves **258 px of 786 432**, in the identical
         box of the one placed unit in all four cross-build pairings, and 0 px outside that box
         and the minimap panel. The FEED is measured separately, on `crowd-static` — 16 unit
         types atlas their textures after the arm, and `main` paints the **25** frames the arm
         seeded while the branch paints **158**, twice each. Two fixtures because neither does
         both halves: `crowd-static` fails the static-frame gate by ~12 000 px, and `one-unit`
         has nothing to append.

         **AND THE REVIEW FOUND THE LARGER HALF.** Unpinning `restored` made a stand-down
         reachable that blanks the whole pass for a SESSION — five one-way `s_rjTried` latches,
         plus `rlistRepaint` being a constant 0 so every generation change blanks it routinely —
         against a contract `atlas_rgb_build` had already written down: a restored-twin failure
         loses restored frames, never the pass. A missing twin now clears the flag and draws the
         indexed atlas, which is this row's own "no second backend" argument carried through.
         Full method and findings, [gpu-status](gpu-status.html) §2.79.

         Deferred to **11-5e-2b**: the RGB read-back, the `mirrorRgb*` fields and the
         `atlasRgb*` publications — plus `tagpu_posedraw.c`'s `restored` flag, which `a->rgb`
         now pins to 0. `tagpu_gaf_atlas_mirror_rgb_step` is reduced to its one reachable
         branch and labelled; the rest stands until its consumers can go with it. (**Part 1
         of that landed 2026-09-19**, below. The `restored` flag did not: it is 11-5e-2c's,
         with the other two pins.)

         It also carried the sixteen-function reset cascade 11-5e-1 orphaned, fourteen of which
         that landing deliberately left standing
         (`tagpu_native_glreset` and four surviving siblings — `scaffold`, `r3d`, `gui`, `fps`;
         the fifth, `tagpu_overlay_glreset`, went with the capture. Through native: the
         restorer's, the effects', the feature pass's, the terrain pass's, the shadow pass's,
         the hires pass's — and `tagpu_hires_glreset` one level below that — and the two
         posedraw resets. The first draft of this parenthetical said five siblings and left
         four of the children out; round 3 counted it.) The cascade can only be
         deleted once the objects it resets are gone, so its tail is behind the escalation
         with them. Carry 11-5e-1's survey finding into it: `tagpu_gaf_atlas_mirror_rgb` can
         only ever refuse — lever on, it is never asked; lever off, there are no GL entry
         points, so it latches `mirrorRgbFailed` and logs a line that reads as a driver fault
         on a perfectly normal run.
       - **11-5e-2b — the restored twin's read-back. BOTH PARTS LANDED 2026-09-19.** The twin
         reached a Vulkan pass two ways — as TEXELS read back with `glReadPixels` and handed
         over as `atlasRgb`, or as the frame LIST the other lane paints for itself — and the
         first cannot happen in any process. Every link re-derived here rather than inherited:
         `oglu_load_dll` has **no caller** (its definition and its declaration are its only two
         occurrences, and it holds the tree's only `LoadLibraryA("opengl32.dll")`), so the
         module is never in the process, so `tagpu_gaf_atlas_mirror_rgb` returns at its entry
         guard **before its own `calloc`**, so `a->mirrorRgb` is NULL; `a->mirrorRgbRows` is 0
         independently, with four writers in `tagpu_gaf.c` and every one assigning 0; so every
         producer's publish guard is false and `atlasRgb` was NULL on every published frame of
         every atlas. Over **both parts**, **`atlasRgb*` goes 112 code sites → 5** — every survivor
         `atlasRgbAniso` — and **`mirrorRgb`-or-`mirror_rgb` 102 → 0**, the spelling leaving the
         tree; part 1 alone took them to 29 (30 after its own review's fix) and 57. Both are
         comment- and string-masked so tombstone prose inflates neither, and
         the second counted as a SUBSTRING so the `tagpu_*_mirror_rgb*` function names are in
         it. (The first draft said 68 → 51 under a `\b`-anchored pattern, which does not match
         either spelling inside an identifier — the label named more than the number counted.
         [The landing review, finding 4.]) After part 1 what remained was **25 GUI sites and
         4 `atlasRgbAniso`**; after part 2, **five `atlasRgbAniso` and nothing else**.

         **THE FIND IS A NEAR MISS, and it is THE NAMED RULE's mirror image.**
         `tagpu_r3d_atlas_mirror_rgb_want` is named for the read-back and is also **the only
         caller of `tagpu_gaf_atlas_restore_vk` for the unit atlas** — the feature and effects
         atlases arm their own lists, the unit atlas's arm lives inside this function three
         lines above the mirror's. Deleting the call along with the read-back it is named for
         leaves that atlas with no list, so `restore_want` makes no job, so the twin is never
         painted, so every restored frame stands down — **silently**, because a lane with no
         list stands down rather than complains, and at ordinary zoom with one commander no
         pixel would have moved either. It survives as `tagpu_r3d_atlas_restore_want`. The rule
         is about predicates that encode "the backend is ready" as "the work is possible"; this
         is a NAME that encodes two jobs as one, and a deletion pass reads names. **The oracle
         that would have caught it is the log line, not the picture**: `unit: restorevk --
         (25 entries seeded …)` and `vk: unit: restoring the twin HERE - 25 of 25`.

         **`atlasRgbAniso` STAYS.** It is the ratio the other lane's twin is filtered at — a
         property of that sampler, not of the mirror — the list accessor publishes it, and
         `tagpu_vk_unit.c` stands a frame down on `h.atlasRgbAniso != s_twinAniso`. It is the
         field 11-5e-2 labelled `[PINNED 0]` and unpinning it is 11-5e-2c's.

         **THE GUI KEPT ITS MIRROR THROUGH PART 1, and that was a decision rather than a
         remainder.**
         `tagpu_gui_surf.c` never calls `tagpu_gaf_atlas_restore_vk`, so `tagpu_vk_gui.c` has
         **no list path to fall back to**; its `h.colourTwins` is gated on `s_colValid`, which
         is declared `static int s_colValid = 0;` and has no other writer, so that gate never
         fires either — and `twin_sprite` and `twin_copy` both `return 0` UNCONDITIONALLY, so
         no op ever carries `TAGPU_GUICOL_ON` and `tagpu_vk_gui.c` tests a bit nothing sets.
         Three independent pins, so removing its `atlasRgb` changed no behaviour: it is a
         **protocol change**, not a feature removal and not a plain deletion, because 16 live
         sites read fields whose shape moves and there was no list to put in their place.

         **PART 2 DID IT — 2026-09-19** ([gpu-status](gpu-status.html) §2.78). The consumer
         (`tagpu_vk_gui.c`'s whole `s_ar*` image and its two upload blocks), the hand-over fields
         (`tagpu_gui.h`), the arm and the publication (`tagpu_gui_surf.c`), and then the producer
         itself: `tagpu_gaf.c`'s 40 `mirrorRgb*` sites, `tagpu_gaf.h`'s 10, and **`tagpu_gaf.c`'s
         ONE remaining GL site** — `xwglGetProcAddress` inside `getgl` — which is the only part
         of 11-5e-2b that moves this gate's own count: wide **323 → 322**, narrow unchanged at
         252. This row said "12 wide GL sites" until the tool's wide pattern was corrected:
         eleven of them were `glog`, the file's own logger
         ([gpu-status](gpu-status.html) §2.77). With the last GL name out, `tagpu_gaf.c` drops
         `#include "opengl_utils.h"` and includers go **six → five**, every survivor being
         `opengl_utils.c` itself, `render_gdi.c`'s `g_oglu_version` read, or one of the three
         files behind escalation reason 1.

         **AND THE UI ATLAS IS NOW A CONSUMER OF NOTHING.** It has no read-back and no list,
         because `tagpu_gui_surf.c` never arms `tagpu_gaf_atlas_restore_vk`; restored art cannot
         reach a UI sprite by any route in the tree. That costs nothing today — `twin_sprite` and
         `twin_copy` have returned 0 unconditionally since 11-4b — but it is a gap this gate
         opened and cannot close, because arming a list for the UI is new work, not a deletion.
         **Left standing with it, deliberately:** the colour-twin SUBSYSTEM — `colourTwins`, the
         per-twin `colImg`/`colView`/`fb2`, the `TAGPU_GUICOL_*` bits, the second render pass and
         the four `*2` pipelines. Dead by the same pins, holds **no GL**, and so belongs to
         whoever decides the colour question rather than to this gate.

         **PART 2 MEASURED IN FOUR RUNS, INTERLEAVED** (`e5a`, `one-unit`, 1024×768, `restorevk.on` +
         `classicpp.on`, `--maxfps 0`), order `main, branch, main, branch`: the two
         **adjacent-in-time cross-build pairs are byte-identical PNGs**, 0 px over the whole
         frame. The fixture flipped mode between the first pair and the second, so both builds
         visited both modes, and the **same-build control across the flip differs by exactly the
         same 44 px** as the cross-build pairs that span it — all of it inside the minimap. The
         build accounts for 0 pixels. Logs: **zero** `VK_ERROR`/`DEVICE_LOST`/`VUID`/validation
         lines across all four runs (the check that matters, because binding 41 now takes
         `VK_NULL_HANDLE` unconditionally), every violation counter 0, and the restore lines an
         identical 16-line multiset in all four. DLL 1 558 016 → **1 547 776** bytes.
         **Four interleaved runs settled what nine batched ones could not**
         ([gpu-status](gpu-status.html) §2.78). **THOSE FOUR RAN WITH THE UI LAYER OFF**, which
         their own logs say, so they covered the world passes and not this landing's subject; a
         second interleaved four with `gui.on=1` covers it. There the layer is armed
         (`leaves=17/17`, `twins=3`, ~580 000 `twin_sprite` and ~4 400 `twin_copy` calls per
         run), twelve grabs factor into TWO oscillators — the minimap's 48-px cluster and the
         cursor's single pixel at (512, 384) — and with both masked **all twelve are one hash**
         over 733 323 compared pixels. The counter dump states two of the three pins as numbers
         on both builds: `colvalid=0` and `col=0/3`, across over half a million calls to the two
         functions that would have armed the colour twin. **And "nothing observable changes" is
         overstated by exactly one log line**: `main` writes `gui: no glReadPixels/FBO entry
         points …` once per process and this branch writes it never — the only line the landing
         removes, and an improvement, but named rather than left to be discovered.

         **PART 1 MEASURED over nine runs** (`e5a`, `one-unit`, 1024×768, `restorevk.on`): five of
         `main`'s `ddraw.dll` and four of this branch's, at `--maxfps 0` and `--maxfps 30`.
         Within a run, 27 pairs, **0 px**. Across builds, all 20 pairs, **0 px outside the
         minimap** — and four of them **0 px over the whole frame**, `main`'s own fifth run
         byte-identical to every branch run. The fixture is **bimodal**: two images 48 px
         apart, all inside the minimap, one unit's dot at a different map position — and
         `main`'s build produced BOTH states, so the build does not pick the mode. Restore log
         lines identical as a multiset in all nine. **A batched control cannot separate the
         build from drift**: the first six runs were three-and-three and read exactly like a
         build difference; it took a fifth run of `main` to see the flip. GL surface unchanged
         at 252/323 (it read 252/366 until the tool's wide pattern was corrected — see
         [gpu-status](gpu-status.html) §2.77); the DLL is 7 680 bytes smaller.
       - **11-5e-3 — the include residue. LANDED 2026-09-19**, and it is
         **`tagpu_fps.c`, `tagpu_scaffold.c` and `tagpu_render3do.c`** — not the three this
         row named. Files including `opengl_utils.h` go **nine → six**; the GL *call* surface
         is unchanged at 252/366, which is the point of the landing.

         **THIS ROW WAS WRONG IN BOTH HALVES AND THE WORK IS WHAT SHOWED IT.** It named
         `render_gdi.c` as residue: it is not — `render_gdi.c` reads `g_oglu_version`, and the
         "files it ends at" list below says exactly that, so the row and the list contradicted
         each other. And it said 11-5e-1 had taken `tagpu_render3do.c`'s include; 11-5e-1 took
         a `tagpu_overlay.h` include out of `tagpu_native.c` — a different file and a different
         header.

         **A CALL COUNT OF ZERO IS NOT "FREE OF THE HEADER".** Of the five files with no GL
         call, only two named nothing from it. `tagpu_render3do.c` took one TYPE, once —
         `GLuint tagpu_r3d_atlas_rgbref(void)` — and `GLuint` is `typedef unsigned int`
         (`inc/glcorearb.h:87`) while `tagpu_render3do.h:14` already declared the function
         `unsigned int`. The definition was the odd spelling out; the change is type-identical
         and the object file proves it (`T` in its own object, `U` in its one caller). Every
         remaining includer now has a stated reason: `opengl_utils.c` defines the entry points,
         `render_gdi.c` reads `g_oglu_version`, `tagpu_gaf.c` is 11-5e-2b's — **taken by its
         part 2 on 2026-09-19, so the list is five** — and the other three are escalation
         reason 1. —
       named rather than counted, because a count goes stale the moment a landing empties
       another file, and because the first version of this line said *five*. That fifth was
       `tagpu_gui_surf.c`, and it was an artefact of the measurement: the script tested
       `"opengl_utils.h" in source` as a SUBSTRING and matched the comment 11-4b left behind
       saying the include had been removed. Mask comments and string literals for the include
       test as well as for the call count — `#include "x.h"` is itself a string literal, so a
       mask applied naively deletes every include instead. After 11-5a, twelve files still
       include it and call GL.

     **The surface as 11-5e-2 leaves it, re-measured on the same masked pattern: 252 GL call
     sites in FOUR files** — `tagpu_hires_draw.c` 104, `tagpu_shadow.c` 87, `opengl_utils.c`
     31, `tagpu_hires.c` 30. That is 540 less 11-5e-2's 288 (`tagpu_restoreglsl.c` 249 and
     `tagpu_gaf.c` 39), and **the four surviving rows are digit-for-digit what they were
     before it**, which is the check that the landing touched only its own files.

     11-5e-1 left it at **540 in six files** — the same four plus `tagpu_restoreglsl.c` 249 and
     `tagpu_gaf.c` 39 — which was 592 less its own 52, with the six rows likewise unchanged. The
     count is taken on both trees with ONE regex in one script rather than quoted from an
     earlier note: a wider pattern (allowing a lower-case letter after the `gl` prefix) reads
     689 on the same tree, because it counts `glyph_raster` and `glreset`, and a number
     compared against one measured differently is not a comparison. **Since 11-5e-2 the count
     is a committed tool rather than a described regex: `tools/gl-sites.py`**, which fixes the
     pattern (`\b(?:gl|x_gl)[A-Z][A-Za-z0-9]*\s*\(`, plus `--wide` for `oglu_|wgl|xwgl`), the
     comment- and string-masking, and the file set (`tagpu/ddraw/src/*.c`, never headers), and
     exits 0 only when the narrow total is 0 — so this row's exit condition can be gated on
     directly instead of re-derived. It reproduces every figure below, on any tree.
     [11-5e-2's review: an exit condition nobody else can reproduce is an assertion.]

     **No world pass and no unit pass is on that list any more**: `tagpu_native.c`,
     `tagpu_terr.c`, `tagpu_feat.c`, `tagpu_fx.c`, `tagpu_scaffold.c`, `tagpu_posedraw.c`,
     `tagpu_overlay.c`, `tagpu_text.c`, `tagpu_ftime.c` and — since 11-5e-2 — `tagpu_gaf.c`
     make no GL call. **That is not the same as GL-free, and `tagpu_gaf.c` is the file where
     the difference matters**: its narrow count is 0 and its WIDE count is 1 — it still
     includes `tagpu_restoreglsl.h`, still resolves `glReadPixels` through `wglGetProcAddress`,
     and still names entry points in a refusal message. Those go with the RGB mirror — and
     11-5e-2b part 1 did **not** move them, because the mirror's last consumer is the GUI
     atlas and that is a protocol change rather than a deletion. [11-5e-2's review.]
     **PART 2 MOVED THEM, 2026-09-19**: `tagpu_gaf.c`'s wide count is now 0 as well as its
     narrow one, the file includes neither `opengl_utils.h` nor any GL name, and the tree-wide
     wide total is **322**. `tagpu_restoreglsl.h` it still includes, and that is correct — the
     name reads as GL and the header is not, supplying `TAGPU_RGLSL_FRAME` and
     `tagpu_rglsl_tileable`, both live and neither GL. **11-5e-2 took its 288; the remaining 252 are `opengl_utils.c` (31),
     `tagpu_hires_draw.c` (104), `tagpu_shadow.c` (87) and `tagpu_hires.c` (30)**, and every
     one of the four is behind escalation reason 1 or waiting on it. The first draft of this
     line said "the two files below" and left the arithmetic one file short: `tagpu_hires.c`
     is the fourth, and it is named here because 30 sites that nobody has listed are how a
     gate's exit condition slips. [The 11-5e-1 review's LOW.]

     **Not covered by 11-5a–e:** `tagpu_shadow.c` and `tagpu_hires_draw.c` (escalation reason
     1) — and, since 11-5e-1 found the dependency, `opengl_utils.c` with them, because it
     defines the entry points those two call. The gate's own exit condition is 11-6's.

     The files it ends at: `opengl_utils.c`,
     `opengl_utils.h` (`tagpu_restoreglsl.c` left this set in 11-5e-2 —
     deleted; note that `tagpu_restore_glsl.h`, with the underscore, is NOT in it and does not
     go: `tools/spirv-gen.py` reads the five restore shaders out of that header to generate the
     Vulkan lane's SPIR-V, and `tools/tascene` extracts the same macros for the browser pack),
     and the plumbing users that
     remain (`opengl_utils.c`'s own `oglu_load_dll`, now called by nothing; `render_gdi.c`'s
     `g_oglu_version`). `render_ogl.c`, `render_ogl.h`
     and the two capture verbs left this set in 11-2.

     **TWO NAMES THIS LIST CARRIED WRONGLY, both checked 2026-09-19 rather than assumed.**

     * **`openglshader.h` WAS NOT DELETABLE ON 09-19, AND THE CLEAN CUT MADE IT DELETABLE ON
       09-20.** The rule it illustrated stands and is the reason to keep reading it: *a file
       whose only consumer is `tools/spirv-gen.py` is load-bearing and invisible to every
       C-level dependency check.* On 09-19 nothing included it as a C header — the single
       `#include` anywhere in `tagpu/ddraw/` was `tagpu_vk_surf.c:36`, taking the *generated*
       `spirv/openglshader.spv.h` — but `spirv-gen.py` read `inc/openglshader.h` through
       `HEADER_SOURCES` to generate `surf_pal` (`PASSTHROUGH_VERT_SHADER` +
       `PALETTE_FRAG_SHADER`), the base blit that put TA's 8-bit surface on the frame through
       the palette, and deleting the header failed `spirv-check` and took the Vulkan lane's
       bottom layer with it. **THE CLEAN CUT DELETED THAT BLIT** ([gpu-status](gpu-status.html)
       §2.81): `surf_pal` came out of `PROGRAMS`, `openglshader` out of `HEADER_SOURCES`,
       `spirv/openglshader.spv.h` was removed and `tagpu_vk_surf.c` no longer includes it.
       **`inc/openglshader.h` now has no consumer of any kind** — checked 2026-09-20, the only
       hits anywhere in `tagpu/` and `tools/` are four comments in `spirv-gen.py` *about* its
       removal. It survives only because deleting a vendored file was not this landing's
       business; it is a free deletion for whoever takes item 11-2.
     * **`tagpu_ftime.c`'s `xwglGetProcAddress` is already gone.** The file includes
       `windows.h`, `stdio.h`, `stdlib.h` and `tagpu_ftime.h` and nothing else; it contains no
       `wgl`, no GL entry point and no `opengl_utils.h`. 11-5e-1 took it with that file's other
       GL residue. The line stayed behind.

     **SURVEYED AGAIN 2026-09-19, AFTER DECISION 1, AND THE DELETION IS FOUR LANDINGS
     RATHER THAN ONE — because three of the four files are not what the row assumed.**
     Every claim below was checked with comments and string literals masked
     (`count_masked.py`), because a plain grep is wrong in both directions here and caught
     this session out once already: the first pass of this survey reported four surviving
     files as including `opengl_utils.h` when the hits were prose *about* its removal.

     **THE REAL INCLUDE COUNT IS FIVE, AND FOUR OF THE FIVE ARE THE DOOMED FILES.**
     `#include "opengl_utils.h"` appears as a directive at `opengl_utils.c:3`,
     `tagpu_shadow.c:38`, `tagpu_hires.c:62`, `tagpu_hires_draw.c:60` and `render_gdi.c:6`.
     **`render_gdi.c` is the only survivor**, and its whole use of the header is
     `g_oglu_version` at `:34` — a cosmetic string spliced into the software-rendering
     warning. `oglu_init` is the only writer of that global and has **no caller**, so it is
     the empty string for the life of the process and `strlen("") > 10` already selects the
     empty branch. Replacing it with `""` is behaviour-identical by construction.

     **AND THE MAKEFILE NEEDS NO EDIT AT ALL.** `Makefile:57` is
     `SRCS := $(wildcard src/*.c) $(wildcard src/*/*.c) res.rc` — no file is named
     individually, so deleting a `.c` removes it from the build with no list to keep in
     step. `thread-split.allow` names none of the four either.

     **THE THREE FEATURES THAT ARE ALREADY DARK, AND HOLD EACH OTHER UP.** This is the
     finding that makes the deletion four landings, and it is not a deletion consequence —
     it is the state of the shipped lane today, measured on the live logs of all four arms
     of 11-5e-2c's A/B:

     1. **The shadow map cannot be drawn.** `s_live = 1` occurs once, at
        `tagpu_shadow.c:403` inside `tagpu_shadow_begin`, and `s_pubHave = 1` once, at
        `:531` inside `tagpu_shadow_end`. **Both functions are callerless** — with
        comments and strings masked, `tagpu_shadow_(begin|end|hills|unit|note_casters|
        caster|locate|apply)(` appears only in `tagpu_shadow.c` and `tagpu_shadow.h`. So
        `tagpu_shadow_live()` returns 0 for the session, both producers publish
        `shadowOn = 0` (`tagpu_posedraw.c:622`, `tagpu_terr.c:1420`), and
        `tagpu_shadow_handover` fails its first term every frame — which makes
        `tagpu_vk_shadow_prepare` return 0 at `tagpu_vk_shadow.c:788`, leaves `s_liveHave`
        unset, and makes `tagpu_vk_shadow_ready()` answer false always. **Both halves went
        dark together, which is why nothing stands down and nothing logs**: the two consumer
        refusals (`tagpu_vk_unit.c:2670`, `tagpu_vk_terr.c:1407`) are themselves gated on
        `shadowOn`. The tree knew half of this — `tagpu_vk_shadow.c:1024` records that
        `ours` is always 0 because `TAGPU_PDHAND.depthOn` lost its producer to 11-5d — but
        "shadows with no casters" and "no shadow map at all" are different sentences and
        only the first was written down.
     2. **Replacement glTF models are loaded and then thrown away, every session.** The
        crowd-static logs of all four A/B arms, both builds, carry
        `hires draw: missing GL proc` followed by `hires: the replacement pass failed to
        build (see 'hires draw:' above) - replacement units fall back to the engine's own
        3DO`, and between them `hires: hires\armpw.glb loaded, 1577 tris, 6 materials, 10
        images, 12 pieces`. `ensure()` (`tagpu_hires_draw.c:403`) resolves its five entry
        points through `getgl`, `opengl32.dll` is never in the process, so `s_state = 2`,
        `tagpu_hires_draw_ready()` is 0, and `tagpu_native.c:2787` nulls **every**
        `n2->hires`. `nhi` is therefore always 0 and `hunits[]` (`:3227`) is read only by
        the emptiness test at `:3366`.
     3. **The Vulkan hires caster cannot fire either**, and for a second independent
        reason: `tagpu_hires_draw` and `tagpu_hires_depth` have **zero call sites** in the
        tree, and `tagpu_hires_depth` is the only writer of `s_hiHave`, so
        `tagpu_hires_handover` returns 0 at its only caller `tagpu_vk_hires.c:677`. Its
        consumers are `tagpu_vk_shadow.c` (which cannot fire, per 1) and `tagpu_vk.c`.

     **What this means for the deletion: it is provably behaviour-neutral, and its oracle is
     0 px on every fixture rather than a difference to explain.** No `renderer=` value
     reaches the GL lane (`dd.c:1908-1931`), `oglu_load_dll` has no caller so no GL entry
     point is resolvable, and the three features above are already producing nothing.

     **What it means for the GATE is a question this row cannot answer, and it is
     escalation reason 1.** Option A keeps `tagpu_vk_shadow.c` and `tagpu_vk_hires.c` "as
     the foundation"; the measurement above says those are passes that *cannot currently
     fire*, and reviving either is a feature landing with a producer to write, not a
     deletion. **Whether 11-6 may close with soft shadows and replacement models dark on
     the shipped lane is the owner's call**, and nothing here rewrites the exit condition.

     **`tagpu_hires.c` IS NOT DELETABLE WHOLESALE, and the row assumed it was.** Of its ten
     entry points, `tagpu_native.c` calls four — `tagpu_hires_mesh` (`:2778`),
     `tagpu_hires_npiece` (`:1172`, `:3233`), `tagpu_hires_gen` (`:1173`) and
     `tagpu_hires_piece` (`:1189`) — and all four are **pure CPU**: the glTF name table,
     the file-stat cadence and the parse. Its 30 GL sites live in exactly four places
     (`mesh_gl_free`, `upload_img`, `white_tex` and `tagpu_hires_vao`'s own body), and
     `tagpu_hires_vao`'s only callers are in `tagpu_hires_draw.c`, which goes. So this file
     is **reduced to its CPU half, not deleted**, and that is the one place in this gate
     where "delete the file" was the wrong instruction.

     **The four landings, each with a result you can run:**

     * **D1 — the GLSL lift.** Move `VS_U`/`VS_H`/`FS_NONE` out of `tagpu_shadow.c` and
       `VS`/`FS` out of `tagpu_hires_draw.c` into headers under `src/` (they must sit there
       rather than `inc/`, because `tagpu_hires_draw.c`'s fragment shader pulls five
       `TAGPU_GLSL_*` macros out of `src/tagpu_glsl.h` and `spirv-gen.py` preprocesses with
       `-Iinc` only), and add two `HEADER_SOURCES` entries at `tools/spirv-gen.py:375`.
       `SOURCES` and `PROGRAMS` are **unchanged** — editing those would delete SPIR-V the
       surviving Vulkan files include. **The gate is exact**: every generated header carries
       a per-shader `glsl <md5>` of the source it was compiled from, so the five hashes
       recorded before the move — `541d516e…` (`FS_NONE`), `3d0b737c…` (`VS_H`),
       `36099c03…` (`VS_U`), `cac5d116…` (`hires VS`), `743af5ef…` (`hires FS`) — must
       come back identical or `spirv-check` fails the build.

       **DONE 2026-09-19, and the gate came back stronger than it had to.** All five
       `glsl` hashes and all five `words` hashes are unchanged. Across the thirteen
       committed headers the only edits are the thirteen `transform` lines — `tool_hash()`
       hashes `spirv-gen.py` itself, so touching `HEADER_SOURCES` invalidates every header
       by design — and two provenance lines now naming `src/tagpu_shadow_glsl.h` and
       `src/tagpu_hires_glsl.h`. **The shipped binary is unchanged**: `.text`, `.rdata` and
       `.data` are each byte-identical to the pre-landing clean build, and the whole DLL
       differs in **six bytes** — the COFF `TimeDateStamp` (`0x88`), the optional header's
       `CheckSum` (`0xd8`) and the export directory's own timestamp (`0x166c04`, four bytes
       into `.edata`). That is why D1 carries no pixel A/B and no review: there is no
       behaviour to measure or to read. `STR`/`STR2` moved into the hires header with the
       shaders, because the stringify is part of the shader text — left behind in the
       deleted file, `uniform vec4 uPiece[TAGPU_HMAXPIECE*3]` would have reached the
       generator unsized.
     * **D2 — the shadow deletion. DONE 2026-09-19.** `tagpu_shadow.c` and `tagpu_shadow.h`
       gone with 87 of the 252 narrow sites; `gl-sites` is **165 narrow / 234 wide**, and
       `tagpu_shadow_[a-z_]+` masked-matches **0 times** in the tree. `TAGPU_SHADOWHAND` moved
       into `tagpu_vk_shadow.h` with the measurement written beside it, and the hand-over is a
       named file-local stub returning 0 — a function rather than an early exit, so a reviver
       changes one body. Measured on `one-unit`, four interleaved arms: **44–45 px cross-build
       of which 44 are the minimap**, the remainder the cursor's own pixel at (512,384), and
       the derived difference box collapsed to that single pixel — nothing outside the minimap
       and the cursor differs at all. The same-build control carries 48 px of the same minimap
       drift. [gpu-status §2.80.] The original scope follows: `tagpu_shadow.c` and `tagpu_shadow.h`, and the eight
       live call sites, each of which returns a constant today: `tagpu_shadow_live()` → 0
       (×2), `tagpu_shadow_mat()`/`_scale()` unreachable behind `if (shadowOn)` (×2 each),
       `tagpu_shadow_handover()` → 0 (×1), and `tagpu_shadow_glreset()` (×1) whose caller
       `tagpu_native_glreset` is itself dead and says so at `tagpu_native.c:3607`. Also
       corrects `tagpu_native.c:3041-3052`, which justifies filling `TAGPU_FXVIEW fv`
       unconditionally on the ground that *"`tagpu_shadow_begin` is handed this same struct
       further down"* — there is no such hand-off, and the uninitialised-stack defect that
       comment records (FOUND 2026-09-15) can no longer occur. The fill stays; the reason
       is rewritten, because the gathers read `fv`.
     * **D3 — the hires deletion. DECIDED BY THE OWNER 2026-09-19: "disable glTF for now",
       "mark glTF implementation: TODO, out of scope". DONE 2026-09-19.** That settles the
       one question this survey could not: `tagpu_hires.c` is **deleted whole**, not reduced
       to its CPU half. 2 588 lines out, 158 in; `.text` down 35 648 bytes and the DLL down
       51 712; **`gl-sites` goes 165 → 31 narrow and 234 → 98 wide, in ONE file** —
       `opengl_utils.c`, which is all D4 has left. The cascade out of `tagpu_native.c` is the
       part the row could not have predicted: `hires_pose`, `pmap_for`, `name_eq`, the `HPMAP`
       cache, the `HPOSE_MAX` pose arena, `hunits[]`, `nhi` and its term in the emptiness
       test, the `hires` field, and `gfx`, whose only reader was the dead branch. Verified by
       the non-static inventory against a clean `git archive HEAD` tree: **identical**, 15 and
       6 symbols. The four-arm interleaved `one-unit` A/B — **re-run on the committed
       review-fix tree, because those fixes took 176 bytes off `.text` and a measurement is not
       inherited across a binary change** — is **0 px outside the minimap and the fixture's own
       oscillating cursor pixel** on all four cross-build pairings, census 749 452 on every arm,
       0 VK errors, and the only log difference is the two LOADER `hires` lines gone
       from the branch arms. That fixture is an ARMCOM with no replacement mesh, so it never
       reached the GL draw path; the lines proving THAT half dead are D2's, not D3's. [gpu-status §2.80.]

       **glTF REPLACEMENT MODELS ARE OUT OF SCOPE FOR THIS GATE AND FOR THE ONES AFTER IT,
       UNTIL THE OWNER SAYS OTHERWISE. [TODO]** They are not being *broken* by this: the
       measurement above is that they already draw nothing on the shipped lane and have not
       for several landings, so the deletion removes a loader whose output is discarded and a
       draw path that cannot resolve a GL entry point. What is genuinely given up is the
       CODE — the glTF parser, the piece table, the COB-driven pose (`hires_pose`), the
       material grouping — and it is given up to `git`, not lost: it is in the tree until
       this landing's parent, and this paragraph is the pointer a reviver needs.

       **`tagpu_vk_hires.c` is KEPT**, as Decision 1 said, and it is a shell after this: its
       hand-over producer goes with `tagpu_hires_draw.c` and its mesh source with
       `tagpu_hires.c`. It stays because `tagpu_vk_shadow.c` and `tagpu_vk.c` call six of its
       entry points, and deleting it is a cascade this gate has no reason to start.

       The original reduction plan, which the decision replaces, follows for the record: `tagpu_hires_draw.c` and its header go whole;
       `tagpu_hires.c` keeps its glTF half and loses `tagpu_hires_vao`, `upload_img`,
       `white_tex`, `gl_probe` and the GL body of `mesh_gl_free`. `tagpu_native.c:2785`'s
       `tagpu_hires_draw_ready()` gate goes with the pass it guards.
     * **D4 — `opengl_utils.c` and the GL headers. DONE 2026-09-19.** `opengl_utils.c`,
       `inc/opengl_utils.h`, `inc/glcorearb.h`, `inc/wglext.h`, `inc/KHR/khrplatform.h`, plus
       the two lines in `render_gdi.c`. `inc/openglshader.h` was **not** in this set, and
       rightly so on the day — see the correction above. The measurement that spared it still
       reads: that header has **zero** `#include` directives naming it anywhere in the tree, so
       every C-level dependency check sees an orphan; walking `SOURCES` + `HEADER_SOURCES` and
       everything reachable under `-Iinc` reached **62 files**, `openglshader.h` among them and
       none of D4's five. It was `HEADER_SOURCES` that held it, and the clean cut took that
       entry out the next day.

       **7 619 lines of files plus four lines out of `render_gdi.c` and thirteen in — 7 623
       deletions, 13 insertions. `gl-sites` goes 31 narrow / 98 wide → 0 / 0: no file in the
       tree contains a GL call.** `.text` −4 816, the DLL −7 680. `inc/KHR/` goes with its only
       file.

       **The safety argument, established before the edit rather than after.** With comments AND
       string literals masked, every `(g_oglu_\w*|oglu_\w*|xwgl\w*|wgl[A-Z]\w*)` **in `tagpu/ddraw`**
       — 268 of them at `main`, 0 at HEAD — is in those files plus `render_gdi.c`'s two
       (`tagpu/src/tagpu.c:55`'s live `wglGetProcAddress` is the other DLL, not this one); `tagpu_overlay.c`, `tagpu_gaf.c` and
       `tagpu_vk.c` look like callers to a plain grep and are all comments. `oglu_init` and
       `oglu_load_dll` masked-match four times between them: two prototypes, two definitions,
       **no callers**, so `opengl32.dll` is never loaded and `g_oglu_version` — a file-scope
       `char[128]` written only inside `oglu_init` — held its zero initialisation for the life
       of every process. The one player-visible consequence was checked in the binaries rather
       than argued: `gdi_render_main` IS reachable, its driver warning's parenthetical was
       already rendering empty, and the literal keeps the `()` so the text is identical
       character for character. `render_gdi.o`'s non-static inventory matches main's and the
       DLL's export count is unchanged at 22. [gpu-status §2.80.]

     Deleting all of it takes `tools/gl-sites.py` to **0 narrow and 0 wide**: the four files
     hold 252 of the 252 narrow sites and 322 of the 322 wide ones.

     **SURVEYED 2026-09-19, while 11-4c's review ran, and the shape is better than the row
     assumed. Three facts, each measured rather than estimated:**

     1. **`tagpu_restoreglsl.c` — 249 GL call sites, the largest GL file left — is ALREADY
        unreachable on this lane, by an ordering, and both of its producers say so.** The GAF
        path bails at `tagpu_gaf.c`'s `if (a->restoreFailed || !a->tex || !pal) return;`, and
        `a->tex` is 0 for the life of the process since 11-4c; so no job is created, `a->rgb`
        is never made, and `twin_mips` and `dump_if_armed` are unreachable behind the same
        gate. The terrain path bails at `tagpu_terr.c`'s `restore_step`, on `!s_atlasTex`, and
        `s_atlasTex` is created only inside `if (!tagpu_vk_owns_present())`. The remaining
        `tagpu_rglsl_step()` calls — **three blocks in two files**, two in `tagpu_native.c`
        (the in-gather step and the `tagpu_rglsl_step_forced` poll) and one in
        `tagpu_gui_surf.c` — step a scheduler with no jobs in it. (All three went with the
        backend in 11-5e-2, and with them the `tagpu_rglsl.step` lever the ta-drive skill used
        to recommend.) **This is what makes the deletion statable**: not "nothing seems to
        call it", but "every producer of its inputs has already stood down, and here is the
        gate each one stops at". It also means the file's 249 calls are not 249 units of work.
     2. **Five files already make no GL call at all** — counted with comments and string
        literals masked, because the raw grep is wrong in both directions here (`glyph*` in
        `tagpu_gui_surf.c` matches, `x_glGenQueries(` in `tagpu_ftime.c` does not).
        `tagpu_scaffold.c`, `tagpu_fps.c`, `render_gdi.c` and `tagpu_gui_surf.c` have no GL
        call, no GL type and no GL constant left, so their `opengl_utils.h` include is pure
        residue; `tagpu_render3do.c` has none either except `GLuint` on two accessors, and one
        of those — **`tagpu_r3d_atlas_texref`, with no caller anywhere** — is the same shape as
        the `tagpu_scaffold_texref` 11-4c deleted. (`tagpu_posebake.c` was the sixth and its
        include went with 11-4c's review fix.) That is a small, purely subtractive first step
        with a build gate on it, and it makes the remaining includer list an honest measure of
        the GL surface instead of an overstatement.
     3. **The four surviving world-pass `init_gl` calls are each inside a lane gate their own
        comments quote** — `tagpu_feat.c`, `tagpu_fx.c` and `tagpu_terr.c` under
        `if (!tagpu_vk_owns_present())`, `tagpu_native.c` under `if (gl_draws)`. The fifth,
        `tagpu_restoreglsl.c`'s, is covered by (1). The sixth is `tagpu_shadow.c`'s, which is
        NOT lane-gated — and does not need to be, because **`tagpu_shadow_begin` has no caller
        anywhere in the tree**: every remaining hit is prose or its own declaration. See the
        escalation note on that file; this is the measurement behind it.

     **Also inherited from 11-4c's review:** `tagpu_text_tex()` (`tagpu_text.c:590`, nine GL
     calls) lost its last caller in 11-4c, when the `glBindTexture` that used it went out of
     `tagpu_fps.c`. Nothing was lost with it — the Vulkan-facing counter is `s_agen`, bumped by
     the atlas writer, not by the accessor — and this row already scopes the GL-object
     accessors here; recording it only because the sentence predates the landing that orphaned
     it.
   * **11-5f — TA'S OWN FRAME LOSES ITS UNITS IN THE VULKAN COMPOSITE. Opened 2026-09-19 by
     landing 11-5d's measurement, and it is a DEFECT rather than a deletion**, so it is its own
     item and not a footnote to 11-5e. What was measured, on `one-unit` with the roster giving
     the subject's screen position ([gpu-status](gpu-status.html) §2.72): the engine
     rasterises every unit on both lanes; `tacli shot` — TA's own surface — carries the
     commander in colour on both; on `renderer=gdi` the presented frame HAS it and the two
     captures are the same picture; on `renderer=vulkan` the presented frame does NOT, with our
     unit pass disarmed and with our terrain pass disarmed as well.

     **So the engine is a real fallback and our composite is dropping it**, which matters beyond
     units: it is every frame, on the shipped lane, with no error and no log line, and it
     applies to anything TA draws that we do not — the shell and the menus (where `tagpu_gui.off`
     is supposed to still show a game), husks the wreck pass does not own, and whatever an
     unarmed pass leaves to the engine.

     **THE LEAD, from reading and NOT yet measured.** `tagpu_surf_take` (`tagpu_surf.c:32`,
     called from `tagpu_overlay_draw`) copies `g_ddraw.primary->surface` on the RENDER thread.
     `ss_shot_service` (defined `screenshot.c:157`, called from `tagpu_overlay.c:360` — this said
     `tagpu_overlay.c:499`, and that file is 365 lines long) reads the same object at the ENTRY of the engine's
     flip on the GAME thread, where its own comment says it holds *"the frame the PREVIOUS flip
     presented"*. `tagpu_surf.h`'s argument is explicitly a LIFETIME one — `g_ddraw.cs` keeps
     the pointer live, `dds_Flip` swaps inside it so the row loop never splices two buffers —
     and says nothing about the buffer holding a FINISHED frame; TA writes those bytes from the
     game thread without entering that section. A snapshot landing mid-draw gets what TA has
     drawn so far. The UI surviving is consistent rather than contradictory: on the presented
     frame it comes from the GUI pass's mirror hand-over, which `tagpu_surf.h` says was the only
     route TA's surface had before 4c.

     **First measurement:** dump the snapshot's own bytes as a PPM in the frame `tacli shot`
     fires and diff the two. **If it holds, the fix is an ORDERING** — snapshot where the shot
     does, at the flip on the game thread, where the frame is finished by construction, and
     publish it for the render thread — **never a timing mitigation** (`CLAUDE.md`, *Fixes must
     be safe by construction*). This is a change to what one thread reads of another's state, so
     it reviews at **`high`** and its reviewer is briefed on that specifically.

     **Not assumed:** that the snapshot is the cause. The measurement above localises the loss
     to our pipeline and no further.

     **THE OWNER'S HYPOTHESIS, 2026-09-19, AND THIS GATE NOW STOPS FOR THEM.** *"Gray world
     issue: each time I see it I believe this simply the fog of war being full screen. Stop when
     at that fix gate and show it to me."* — so **11-5f does not get driven to a fix without the
     owner seeing it first.** Whatever a session finds here, it stops at the fix and shows the
     picture.

     **The owner then corrected the domain fact that disciplines this**, and it is recorded
     because a session that did not know it would chase the wrong thing: *"I don't think trees
     are supposed to be hidden by the gray fog of war, only the full black one for unmapped map
     areas."* So grey fog keeps static features and merely darkens terrain; black fog draws no
     terrain at all. **A frame with grey terrain and no trees is therefore not any fog state TA
     has**, and the first attempt to read one that way was wrong.

     Three things to say honestly before anyone tests it, and the third is a retraction:

     1. **What is established, and it is narrower than it has been stated.** The AGGREGATE
        saturation of our frame is roughly half the engine's (mean 28.5 vs 14.7) while the
        STRUCTURE matches — the same landforms in the same places, which rules out a wrong region
        or a gross UV error. That is all.

     2. **"We desaturate each pixel" IS ALREADY REFUTED, by this gate, and must not be
        re-derived.** An 18-point sample found both frames carrying grey and saturated pixels in
        *different places*, with **5 of 18 points where OURS is MORE saturated than the
        engine's** — engine `(43,43,43)` grey against ours `(7,83,43)` green; engine `(31,31,31)`
        against ours `(0,128,128)` cyan. Mean `|ours − luminance(engine)|` is 5.5, not 0. The
        leading hypothesis is a **sub-texel sampling offset on TA's dithered terrain art**: a
        fractional offset flips which of two alternating palette entries a screen pixel lands on,
        and any averaging across that pair collapses a saturated pair toward its mean.

     3. **RETRACTED 2026-09-19, by D4's review.** A session measured our frame ALONE, found 84.3 %
        of its world pixels at saturation 0–2 with a median of 0.0, and concluded "everything our
        lane renders is greyscale and the only colour is the engine's". **The histogram is
        correct and the conclusion does not follow** — it never compared the engine's frame at
        the same points, and item 2 above had already refuted it. The same session also claimed
        the engine's composited layer "flickers continuously"; counting all **36** grabs of the
        three A/B runs shows **33 at bit-for-bit the same saturation state**, and two of the
        three anomalies are **screen tears** (divergence starting at row 384 of 768, and row 392,
        with everything above byte-identical — `import -window` has no vsync). Neither claim
        survives. [gpu-status §2.80 carries the counts.]

     **WHAT IS GENUINELY NEW, AND IT IS ABOUT THE HARNESS.** All three anomalous grabs across
     three A/B runs are in a **`brB`** arm, and `meas_x.sh` runs `mainA → brA → mainB → brB`, so
     `brB` is always the **fourth and last** launch. Three for three is arm-correlated rather
     than random, and it is confounded (`brB` is both "last" and "branch") — but D3's and D4's
     anomalies came from *different branch binaries*, which points at launch order. The third
     anomaly is not a tear: it diverges across the whole world viewport and holds 2 782 saturated
     pixels against 74 495, a real whole-frame loss of the engine's layer. **Nobody has tested
     the fourth-launch hypothesis, and it should be tested before it is explained.**

     **What would settle it, and it is cheap:** the question is now "what desaturates our lane's
     output", and saturation is a property of the colour path, not of geometry. Our terrain
     samples an indexed atlas through a palette; a palette that resolves to luminance, an
     `R8`/`sRGB` format mismatch, or a fog/shade factor applied at full strength would each
     produce exactly a median saturation of 0.0. Fog of war is still worth excluding first
     because it is state rather than a guess — `tagpu_fogwide.c` and the two grids are ours, and
     the engine's fog bit is in [exe-reverse-engineering](exe-reverse-engineering.html) — so read
     the grid the Vulkan lane binds on a grey frame and ask whether it says "unseen" everywhere.
     Either way it is a **data check, not another frame diff**, which is what this gate was
     already told to do next.

     **HOW THE OWNER REVIEWS IT, ON THEIR INSTRUCTION 2026-09-19:** *"When you need me to check
     the gray world issue, stop the loop and let me review the running game with shield off to
     confirm the issue."* So the session **stops the loop**, leaves an instance RUNNING with
     `tacli shield <name> off` so their keyboard and mouse reach it, and hands it over — not a
     screenshot, and not on the 4K screen (`tile` in `instance.json` is the lever). Confirmation
     comes from the owner looking at a live game, before any fix is attempted.

   * **11-6 — the exit condition.** `renderer=gdi` documented and MEASURED as the stock
     reference, with the residue named rather than waved at. **Note what 11-5d's gdi control
     already shows about this reference: it renders TA's own units and our lane does not**, so
     until 11-5f closes, a gdi-vs-vulkan comparison is measuring that defect as well as whatever
     it was pointed at.

   **WHAT "STOCK" IS TAKEN TO MEAN — the decision this item was told to make.** The candidate
   readings are *stock as a lane* (nothing of ours reaches the screen) and *stock as a process*
   (the engine's bytes are untouched). **The exit condition is the first**, and the second is
   recorded as false with its two reasons named, because a claim nobody can state precisely is
   worse than a narrower one that is true:

   * `0x4266A7`, the DirectX-version warning's `jne`→`jmp`, is **ungated and stays ungated**. It
     suppresses a modal startup dialog that appears only because the process is a DirectDraw
     shim; it changes no pixel the engine draws and no input the engine reads. It is part of
     making the game start, not part of drawing it.
   * the `tagpu_curs` pair (`0x43E50C`, `0x499041`) **does** change input semantics, is **on by
     default**, and is levered by `tagpu_curs.off`. It exists to compensate for `tools/tacli`
     writing `Interface Type = 1`. So a gdi lane driven by `tacli` is not input-stock unless the
     lever is set, and that is stated wherever the reference is cited rather than left implied.

   So: **`renderer=gdi` is the stock reference for RENDERING**, and `tagpu_curs.off` is the extra
   step for a run that also wants stock input. Anyone measuring against it needs both sentences.

   ~~**BLOCKED ON LANDINGS 10b AND 10c**~~ — **BOTH LANDED 2026-09-18, and this item's exit
   condition can now be CHECKED for the first time.** What the block said:

   > *(the structure-shadow gate, and the fact that no `tacli` verb works on the gdi lane at all —
   > the on-demand trigger family is called only from `tagpu_overlay_draw`, which only the GL and
   > Vulkan backends call. Until 10c, this item's exit condition cannot be checked by anything we
   > have. `renderer=gdi` is not stock today, and the survey narrowed that from a class of
   > suppressors to exactly two bytes: the `je`->`jmp` pair at `0x4592C6`/`0x45952C`, which has no
   > runtime gate to be inert through, so the gdi lane draws no structure shadows and nothing of
   > ours draws them either.)*

   **10b** made the two `je`s gated detours, so the gdi lane draws the engine's structure shadows
   again and the pair is inert there. **10c** put the trigger family and input's token half on the
   engine's flip, so `tacli ui`, `peek`, `units`, `features`, `scenario`, `click` and `keys` all
   answer on that lane — the lane can be observed **and driven**.

   **BUT THE HEADLINE CLAIM IS STILL FALSE, FOR A DIFFERENT REASON, AND THIS ITEM OWNS IT.**
   `renderer=gdi` is not *stock as a lane*: of `tagpu_apply_patches()`'s three patch sites,
   **exactly one is ungated** — `0x4266A7`, the DirectX version warning, which has no lever at all
   — and the other two **are** the `tagpu_curs` pair (`0x43E50C`, `0x499041`), both inside the
   `else` of a `tagpu_curs.off` test, which changes input semantics behind a file.
   [CORRECTED by the landing review of 10c-2: this said "three patches with no runtime gate at
   all, **and** the `tagpu_curs` pair", which triples the ungated count and then counts the pair
   twice on top. It also contradicted this same page's correct table two sections below. The
   number matters because it is this item's exit-condition arithmetic, and an overcount is what
   decides a landing is large.]
   Two further things do not reach gdi and are the **command channel**, not
   the trigger family: the camera hold (`tacli eye`) and `tacli wheel`. The hold rides the record
   `tagpu_cmd_post` publishes, and that is called only from `tagpu_zoom_frame_end`, inside
   `tagpu_overlay_draw`. Deciding what "stock" means against those four facts is this item's work,
   and it is the part 10b and 10c did not do for it.

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

   **AND `tagpu_render3do.c`'s "nothing" DOES NOT MEAN IT GOES WHOLE** [checked 2026-09-18].
   The table above answers one question — *does the Vulkan lane call this file directly?* — and
   for this one file the answer is no, which reads like a file that can be deleted. It cannot.
   Four other passes call it: `tagpu_native.c`, `tagpu_posebake.c`, `tagpu_overlay.c` and
   `tagpu_posedraw.c`, and the last of those calls `tagpu_r3d_atlas_mirror`,
   `tagpu_r3d_atlas_mirror_rgb`, `tagpu_r3d_atlas_mirror_want`, `tagpu_r3d_atlas_mirror_rgb_want`,
   `tagpu_r3d_atlas_mirror_rgb_step`, `tagpu_r3d_atlas_restore_list` and `tagpu_r3d_lut_mirror`
   from inside **`pd_view_publish`** (`tagpu_posedraw.c:744`) — the function that fills the
   hand-over payload the Vulkan lane reads. Those mirrors ARE the CPU-side twins landing 2 built
   for that lane. So `tagpu_render3do.c` is a **producer for Vulkan by one hop**, and what 11-2
   takes out of it is its ten GL calls — one LUT texture, created at `:298`–`:304` and updated
   at `:136`–`:139` — and nothing else. The same one-hop question is owed to every other file
   in the table before a line of it is deleted.

   **THE SYMBOL-BY-SYMBOL SURVEY FOR 11-2** [2026-09-18]. Every non-static `tagpu_*` function
   the eight files define was matched against every other source, with comments and string
   literals stripped. The shape is the same in all eight: **exactly ONE symbol per file is called
   by the Vulkan lane** — its `_handover`, or `tagpu_native_worldtgt` — and everything else that
   survives is called by a pass on the GL side. What makes the deletion surgical rather than
   mechanical is that several of those callers **run on every lane**, so their callees are
   keepers even though no `tagpu_vk*.c` names them:

   | kept because … | the symbols, and who calls them |
   |---|---|
   | the frame packet's publisher runs on every lane | `tagpu_native_owns_unit`, `tagpu_native_want_builds` ← `tagpu_packet_pub.c` |
   | the engine-suppression layer runs on every lane | `tagpu_native_owns_obj`, `tagpu_native_wrecks_armed`, `tagpu_posedraw_live`, `tagpu_posedraw_refused` ← `tagpu_owndraw.c` |
   | the marker/order passes read unit state | `tagpu_native_unit_pos` ← `tagpu_mark.c`, `tagpu_order.c`; `tagpu_native_selbox_complete` ← `tagpu_markown.c` |
   | the sound pass drives the effect emitters | `tagpu_fx_caps`, `tagpu_fx_emit_dot`, `tagpu_fx_emit_frame`, `tagpu_fx_set_mute`, `tagpu_fx_tile_visible` ← `tagpu_sfx.c` |
   | the terrain key is the composite's own constant | `tagpu_terr_key` ← `tagpu_gui_surf.c`, `tagpu_markown.c`, `tagpu_terrown.c` |
   | the fog LUT and the scissor are asked by five passes each | `tagpu_native_foglut`, `tagpu_native_scissor_on` ← `tagpu_feat.c`, `tagpu_fx.c`, `tagpu_mark.c`, `tagpu_posedraw.c`, `tagpu_terr.c` |

   What is left over after those is the delete set, and it has a shape too: the `_render`,
   `_glreset` and draw entry points (`tagpu_posedraw_begin`/`_unit`/`_end` and their ghost,
   shadow, slant and wire variants; `tagpu_hires_depth`/`_draw`; `tagpu_shadow_begin`/`_caster`/
   `_unit`/`_end`/`_hills`), every one of them reached only from below `tagpu_native.c:3817`.
   `tagpu_native.c` itself keeps fourteen exported symbols and loses its draw body.

   **Re-checked 2026-09-18, for the eight files landing 11-2 owns, and the table holds.** Worth
   recording is HOW it failed first: deriving each file's exported symbols from its own header
   and testing those against the Vulkan sources reported `tagpu_terr.c`, `tagpu_feat.c`,
   `tagpu_fx.c` and `tagpu_shadow.c` as called by **nothing**, which would have made four of them
   look deletable. Grepping the names directly found `tagpu_terr_handover` in `tagpu_vk_terr.c`,
   `tagpu_feat_handover` in `tagpu_vk_feat.c`, `tagpu_fx_handover` in `tagpu_vk_fx.c` and
   `tagpu_shadow_handover` in `tagpu_vk_shadow.c` — the header extraction, not the table, was
   wrong. The same pass's apparent *extra* findings (`tagpu_native_foglut`,
   `tagpu_posedraw_depth_begin`, `tagpu_hires_draw`…) turned out to be mentions inside comments,
   with no call site in any `tagpu_vk*.c`. **This is the third time this item's survey has been
   caught by its own boundary**, and the rule it keeps re-teaching is the one at the end of the
   method note: a mechanical scan is only as good as the boundary nobody checked, so a name that
   decides whether a function can be deleted gets grepped for directly.

   **So landing 11 is `tagpu_restore_core.c`'s split, sixteen times.** Each of those files is a
   PRODUCER the Vulkan lane depends on with a GL DRAW half bolted to it, and the landing deletes
   the half, in place, leaving the producer and its `_handover`. Not one of them is a file that
   can be removed. The six files the row names are the only ones that go whole, and they are the
   small part.

   `tagpu_native.c:3817`'s `if (!gl_draws) { …hand over…; return; }` is what makes this
   tractable: **everything below that line in the unit pass is GL-only by construction**, which
   is why `tagpu_shadow.c` and `tagpu_hires_draw.c` carry GL with no lane guard of their own —
   their draw entry points are called from below it (`tagpu_shadow_caster` `:4247`,
   `tagpu_shadow_unit` `:4278`, `tagpu_hires_depth` `:4287`, `tagpu_hires_draw` `:4599` and
   `:4658`) while their hand-over entry points are called from the Vulkan lane. [The four numbers
   this paragraph carried — `:3753`, `:4155`, `:4463`, `:4522` — were taken before landings
   10b and 10c and had all drifted; `:4155` had become a comment line and `:4463` a macro
   definition. Re-derived 2026-09-18.] The seam inside
   each file is real and already drawn; it is just drawn sixteen times.

   The other headers are cleaner than that — **this paragraph is the survey taken BEFORE
   landing 11-1, and its `d3d9` half has since been carried out**: `openglshader.h` and
   `d3d9shader.h` had **no includer outside the set at all**; `render_ogl.h` has four
   (`config.c`, `dd.c`, `fps_limiter.c`, `winapi_hooks.c`) and `render_d3d9.h` had five (those
   plus `utils.c`, `wndproc.c`), all of them the renderer-selection surface rather than drawing.
   `d3d9shader.h` and `render_d3d9.h` are gone; the `render_ogl.h` figure is 11-4's.
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

   **AND THE METHOD HAS A BLIND SPOT THAT THIS LIST WALKS STRAIGHT INTO
   [found 2026-09-19, by landing 11-4a's build gate].** Stripping string literals is the right
   move for finding *callers* and the wrong one here: some of this repo's string literals **are
   consumed at build time**, so a symbol with no C user can still be load-bearing. Concretely —

   **`openglshader.h` MUST NOT BE DELETED — true on 2026-09-19, and no longer true since the
   clean cut.** Its two shaders, `PASSTHROUGH_VERT_SHADER` and `PALETTE_FRAG_SHADER`, were the
   source of truth for the `surf_pal` program in `tools/spirv-gen.py`'s PROGRAMS table. The build
   translated them into `inc/spirv/openglshader.spv.h`, and **`tagpu_vk_surf.c:36` included
   that** — TA's 8-bit surface resolved through the palette, *the frame's bottom layer* (§2.52,
   landing 4c-1). Deleting the header would have taken the shader the Vulkan lane composited
   everything else on top of. **The clean cut deleted that composite**
   ([gpu-status](gpu-status.html) §2.81), and with it `surf_pal`, the `HEADER_SOURCES` entry, the
   generated header and the include — so the header is now an orphan at every level and a free
   deletion for 11-2.

   **The blind spot is what to keep from this, not the verdict.** No symbol search could have
   caught the dependency while it existed: the consumer was `spirv-gen.py`, which is not a `.c`
   file and does not call anything. A symbol search would have been wrong on 09-19 and right on
   09-20, for reasons it could not see either day.

   The same applies to `tagpu_restore_glsl.h` — note the underscore; it is **not** the
   `tagpu_restoreglsl.c` this item lists. `spirv-gen.py`'s `RESTORE_HDR` reads its
   `TAGPU_RESTORE_*_{VS,FS}` macros and `tagpu_vk_restore.c:88` includes the result. The `.c` is
   in scope for deletion; the `.h` is not.

   **The rule for the rest of 11-5: before deleting any file, check it against the PROGRAMS
   table and `RESTORE_HDR`.** **Ten** source files carry manifest-named shaders as of 2026-09-20
   — `tagpu_native.c` (6), `tagpu_shadow.c` (3), and `tagpu_feat.c`, `tagpu_fps.c`, `tagpu_fx.c`,
   `tagpu_hires_draw.c`, `tagpu_mark.c`, `tagpu_posedraw.c`, `tagpu_scaffold.c`, `tagpu_terr.c`
   (2 each). It was twelve until the clean cut took `tagpu_gui_surf.c`'s nine and
   `openglshader.h`'s two out of `PROGRAMS` along with the files and the blit they drew. The build refuses rather than corrupts, so this
   is a warning about wasted work and a wrong plan, not about a silent bug — but a plan that
   lists a file it cannot delete is a plan that will be abandoned mid-landing.

   * **`opengl_utils.c` — goes WITH `render_ogl.c`, and the first pass said something wrong and
     more interesting.** It claimed the four shader helpers were "referenced only by
     `opengl_utils.h` — already dead". They are not dead: `oglu_build_program`,
     `oglu_build_program_from_file`, `oglu_ext_exists` and `oglu_init` are all used by
     **`render_ogl.c`**, which the first pass had excluded as "not surviving" and then reported as
     absence. Outside the deletion set only **`oglu_load_dll`** is used (`dd.c`). The practical
     consequence is better than "dead" and different from it: **delete `opengl_utils.c` and
     `render_ogl.c` together and nothing outside the set loses a helper**, with `oglu_load_dll`'s
     one caller the only thing to resolve.
   * ~~**`render_d3d9.c` — four surviving files, not one.**~~ **✓ CARRIED OUT by landing 11-1**;
     kept as the shape of the work the other rows still face. Seven exports; real users in
     `dd.c`, `utils.c`, `winapi_hooks.c` and `wndproc.c`. `d3d9_release_resources` had **no user
     outside the file at all**. No lane involvement — and that is why this one went whole while
     the sixteen passes will not.
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

   **NOT CHECKED:** `render_ogl.h` — a header, which can only be checked by include, which is
   the check this entry warns against relying on. `openglshader.h` was in this line too until
   the clean cut settled it: no `PROGRAMS` entry, no `HEADER_SOURCES` entry, no includer, so it
   is checked now and it is an orphan.

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
* **`terrown`, `featown`, `fxown`, `markown`** — `cursown` was the fifth until the clean cut
  deleted it — each hold a `volatile unsigned char`
  the stub compares, set to 1 only by a `set_skip(ours-live)` call from the pass that paints,
  with a frame heartbeat that restores the engine's draw if the pass stops.
* And **every one of those flags is set from `tagpu_overlay_draw`**, which has exactly two
  callers: `render_ogl.c:1632` and `render_vk.c:232`. **`render_gdi.c` contains no `tagpu_` call
  at all.** So on the gdi lane no flag is ever set, `s_armed` stays at its initial `-1`, and
  every one of those suppressions is inert by construction rather than by luck.

**The one that is not — and since 2026-09-20 it is not armed either.** `tagpu_owndraw_init`
flips two `je`s to `jmp`s — `patch_je_to_jmp(0x4592C6)` and `(0x45952C)` — whenever
`g_armed && g_all`, and **a byte flip has no runtime gate to be inert through**. The clean cut
took `tagpu_owndraw.on` off the defaults table with the other `*own` levers, and the function's
first line is `if (!tagpu_opt_on("tagpu_owndraw.on")) return;`, so at the shipped defaults the
flip does not happen; what follows is what it costs whenever somebody arms the lever. `tagpu_owndraw.h` says what that costs: *"the engine then
draws NO cached slant shadow and the native pass owes every structure one"*. With the shipped
defaults (`tagpu_owndraw.on` = `all`) under `renderer=gdi`, nobody pays that debt, so **every
building loses its slant shadow** and the lane is not stock.

**THAT IS NOT THE WHOLE OF THE GAP, and this sentence used to say it was** [the fourth review of
10b]. `tagpu_apply_patches()` (`dllmain.c`, unconditional at `DLL_PROCESS_ATTACH`, before any
renderer is chosen) applies **three** byte patches, not one:

| VA | what | how it can be inert |
|---|---|---|
| `0x4266A7` | the DirectX version warning, `jne`→`jmp` | nothing — the residual named below |
| `0x43E50C` | `je`→six `nop`s, so the contextual-cursor case always takes the classic branch | a `tagpu_curs.off` FILE only — no lane check, no runtime flag |
| `0x499041` | a **27-byte rewrite** of the left-click dispatch, so a contextual click decides on Interface Type rather than cursor index | the same file |

The `tagpu_curs` pair is on by default and **changes input semantics**, which is strictly larger
than the structure-shadow flip this landing closed — that one only ever removed a shadow. Two
softer items are also unnamed: `tagpu_menu_init()` writes `impure-patch.ufo` into the working
directory and installs a `DrawGameScreen 0x468CF0` observer that adds a render-options screen, and
`tagpu_hud_init()` writes the engine's viewport rect whenever the stored HUD scale is not stock.
(`tagpu_reclaim` is benign there: with its counters stuck at 0 the drain condition is false, so
entries free on the next `FreeObjectState` rather than leaking.)

**Item 11's exit condition is checked against this sentence**, so it mattered that it was wrong:
`renderer=gdi` is *not* stock once 10b lands, it is stock **with respect to structure shadows**.
Closing the rest is either a `tagpu_curs` landing of the same shape 10b just had, or an explicit
decision to document the cursor pair as a deliberate deviation.

**LANDING 10c-1 IS DONE AND THE OBSERVATION HALF OF THIS IS FIXED [2026-09-18].** The five
trigger functions that do not need the frame packet — `tagpu_peek_frame`, `tagpu_weapons_frame`,
`tagpu_ui_frame`, `tagpu_cat_frame` and `tagpu_scenario_frame` — are called from
`tagpu_gui_hook.c`'s `before_flip` now, on the game thread, above that function's three early
returns. `tagpu_overlay.c` keeps one entry point, `tagpu_triggers_frame`, which is what the flip
calls. **VERIFIED BY RUNNING IT** on a `renderer=gdi` instance:

```
gui MAINMENU.GUI  640x480   under: -
focus=SINGLE
  1  button   SINGLE           187,403     ok             s
  ...
```

— a snapshot with click coordinates, where the paragraph below predicted *"no UI snapshot
appeared"*. `tacli units` answers there too, and the GL lane was re-checked for regression and is
unchanged.

**LANDING 10c-2 IS DONE [2026-09-18]: `renderer=gdi` CAN BE DRIVEN.**
`tagpu_input.c` was **split on the packet**, not moved. Its token half — `tagpu_input_frame`:
`tagpu_keys.txt`, the injected pointer, the shield's held-modifier expiries — reads no packet, so
it joined the family and runs from the flip on the game thread. Its camera hold became
`tagpu_input_eye_frame` and stayed in `tagpu_overlay_draw` on the render thread, where `f->packet`
exists and where its answer is read.

**Measured on both lanes.**

| lane | what was run | result |
|---|---|---|
| gdi | `tacli ui` → `click 187 403` → `keys esc` → `keys s` | `MAINMENU.GUI` → `SINGLE.GUI` → `MAINMENU.GUI` → `SINGLE.GUI`. Before this, clicks and keys did nothing at all there |
| vulkan | `tacli scenario load … --restart` | drove the menus **by click** into a live *Two Continents* game, `applied 5 of 5` |
| vulkan | `tacli eye 2400 2400`, then `1200 1200`, then `--release` | eye is **exactly** the point asked for, both times; after the release the camera edge-scrolled 1200 → 1904 → 6704 and stopped when the pointer left the edge |

`viol=0 pviol=0 foreign=0`, no `ErrorLog.txt`.

**THE DESIGN BELOW WAS WRONG AND THE LANDING DOES NOT IMPLEMENT IT.** It is kept because the
reasoning that replaced it is only legible against it. `tagpu_packet_pub_last()` was to give
`do_eye` a packet on the game thread — and the accessor's own analysis is still correct as far as
it goes. What it never asked is **who reads `do_eye`'s output**:

* `do_eye` writes `s_eyeHold`, `s_holdX`, `s_holdY`, `s_holdValid`. The only readers are
  `tagpu_input_cmd()` and `tagpu_input_eye_held()`, and the only caller of the first is
  `tagpu_zoom_frame_end()` — which is called from four places, **all inside `tagpu_overlay_draw`**.
* So moving the poll to the game thread would have made four words cross-thread in order to serve
  a consumer that is not on the other side. That is the exact class `CLAUDE.md` singles out, paid
  for nothing.
* And on the gdi lane it would still have delivered **nothing**: the hold reaches the game thread
  only through `tagpu_cmd_post`, which `tagpu_zoom_frame_end` calls — so on a lane with no overlay
  frame there is no command record, and a hold that were read could not be applied. The camera
  hold cannot work on gdi until the *command channel* moves, and that is not this gate.

**The line the split actually follows is the one the family was already drawn on**: a consumer
that dereferences `f->packet` cannot be called from `before_flip`, because that frame has none.
`tagpu_input.c` had one function on each side of that line and was being treated as if it had one.
No accessor, no lock, no new cross-thread state — and each half keeps its own poll counter, so
each runs at the cadence of the clock it is handed (62.5/s from the flip's time gate, the render
frame counter in the overlay).

**Two safety arguments the landing rests on, both by construction:**

* **No reentrancy.** Every injection leaves `do_keys` by `PostMessageA` (a tagged `WM_TAGPU_*`) or
  by `SendInput`; there is no `SendMessage` on any path out of it. A token handled inside the flip
  detour is therefore *delivered* by the engine's own message pump afterwards — it cannot be
  dispatched into the wndproc halfway through a present.
* **A lifetime.** `s_frame` points at the caller's `TAGPU_FRAME`, and on the game thread that is
  `before_flip`'s **stack**. It is now set immediately before `do_keys` and cleared immediately
  after; every reader (`si_mouse`, and the park in `inject_click_at`) is reached from `do_keys` and
  from nowhere else.

**LANDING 10c-3 IS DONE AND THE GATE IS CLOSED [2026-09-18]: `renderer=gdi` CAN BE MEASURED.**
Driving it was not enough, and the thing that showed that was a `tacli scenario load` on the gdi
lane failing with *"no game after 180s — the shell never reached a live map"* **while the game
behind it had loaded perfectly well**. Only the DETECTOR was missing: `scenario load` waits for
`units: alive=N` in `tagpu.log`, and that line — with the roster dump and `mouse:`, the whole of
what `tacli` greps for besides `peek:` — came from `log_units` in `tagpu_overlay.c`, which the gdi
lane never enters. Proven rather than assumed by running `tacli scenario apply` against the
instance `load` had just declared dead: **applied 5 of 5**.

`log_units` is `roster_log` in `tagpu_packet_pub.c` now, on the game thread, fed by the packet
that file has just filled.

**AND IT CANNOT RIDE ON THE PACKET NAIVELY — this is the trap, and it is the same shape as the
one 10c-2 walked into.** `fill_frame` runs only when a publish is not skipped, and the FRESH gate
skips whenever the renderer has not taken the last packet. `tagpu_packet_acquire` has exactly two
call sites, `render_vk.c` and `render_ogl.c` — **so on `renderer=gdi` nothing takes, every
unforced publish is skipped, and `fill_frame` runs about once per level.** A roster hung off
`fill_frame`, or off the `tagpu_packet_pub_last()` accessor this plan once prescribed, would have
produced nothing at all on the one lane it exists for, and produced it silently. So the publish is
forced when no fill has happened for **`ROSTER_HDR_MS`, the cadence of the slowest line that is
actually read** — self-limiting, because on a lane whose renderer takes packets the fills are
~16 ms apart and it never fires. (This sentence said *"the shortest of the three cadences"* until
the landing review's second round: that was the first cut's rule, and the paragraph below explains
why it changed, twenty lines after a reader would have hit the stale claim.)

**Measured on both lanes, after a `scenario load` and ~45 s of play:**

| lane | `units:` (500 ms) | `mouse:` | roster | exchange counters |
|---|---|---|---|---|
| gdi | 92 | 92 | 97 lines over ~9 blocks, `tacli roster` answers | **not observable** — see below |
| vulkan | 91 | 178 | answers | `overrun=2` `gap=2` of `pub=3582`, `taken=3579` |

A second run was added here as "run B, gdi, 192/376" and **it was not gdi** — see the retraction
below. Its figures were a GL lane behaving exactly as this table's vulkan row does.

The Vulkan row is the no-regression half: `overrun=2` is the two pre-existing forces (the
level-end packet and the level's first in-play one), so **the keepalive never fired on a lane
with a consumer**, which is the property it is built on.

**THE gdi COUNTERS CANNOT BE READ, and that is worth knowing before anyone goes looking.** The
`packet:` heartbeat is emitted by `tagpu_packet_frame_end`, whose only callers are
`render_ogl.c` and `render_vk.c`. On gdi it is never printed. The forced-fill rate there is
therefore stated from the code — at LEAST one per `ROSTER_HDR_MS`, ~2/s — and not from a
counter, and "at least" is doing real work in that sentence: run B below measured a fill rate
above the forced one and there is no counter on this lane to say by how much.

**`mouse:` IS NOT A MACHINE-READ LINE, and an earlier draft of this section built its whole
verification argument on it** [the landing review]. `tacli`'s structured readers are the peek
line, the roster line and `units:`; the only other reference to `mouse: screen=` in the tree is a
sentence of prose in [input-firewall](input-firewall.html). So the ratio `mouse:` = 2 × `units:`
was never "the check that matters" — and it was also **not true on both lanes**: the figures
recorded were gdi 62/124 and vulkan 77/151, and 2 × 77 is 154. The force now asks the HEADER's
cadence instead, which halves the forced-publish rate — the one new risk this landing carries.

**RETRACTED, ONE COMMIT LATER: "the gdi fill cadence is not pinned by that force".** This
section briefly carried a paragraph saying a second run's 2:1 `mouse:`/`units:` ratio showed the
keepalive's force was only a *floor* on the fill rate. **That run was not on gdi.** `tacli
scenario load` calls `write_ddraw_ini` whenever the scenario carries a `setup.res` — `fx-lasers`
carries `1024x768` — and that function writes `renderer=openglcore` unconditionally, so the
instance's hand-set `renderer=gdi` did not survive the load. The tell was on screen and went
unread: the ini said `openglcore` the next time it was opened. A GL lane HAS a consumer, fills
every ~16 ms, and therefore prints `mouse:` on its own 250 ms gate and `units:` on its 500 ms
one — a 2:1 ratio that is the documented behaviour and says nothing about gdi.

So the original statement stands and is what this page says: **where nothing consumes, the only
fills are the forced ones**, one per `ROSTER_HDR_MS`, and `mouse:` reads 1:1 against `units:`
because the mouse gate can fire at most once per fill. The gdi row above is the measurement.

The lesson is the plainer one, and it is in [ta-drive](ta-drive.html) now so the next session
does not pay for it: **a measurement is not of the lane you set, it is of the lane the process
actually ran**, and on this harness the second is written by the tool.

**AND `tacli roster` WAS LABELLING THE BLOCK WITH THE WRONG HEADER** [the landing review, LOW].
`roster_log` emits, in one call, the `mouse:` line, then the dump block, then its `units:`
header — so a block's own header is the first `units:` line BELOW it in the file. `cmd_roster`
scans the log backwards, and it overwrote `eye` on every header it passed and broke on the first
one it met after collecting units: it held the right value for exactly one iteration and then
replaced it with the header from the PREVIOUS call, 500 ms older than the `screen=` values it
labels and taken from a different eye than the projection they carry. The reader now stops
before that overwrite. Verified on a real gdi log by rewriting every header's eye to its own line
number: the old reader returned line 1200, the new one line 1206, and 1206 is the block's own
(1201..1202); the unit list is identical either way. This was not introduced by the move —
`log_units` had the same shape on the render thread — but it is the reader for a line this
landing rewrote, so it is fixed here rather than left.

That is a cost question rather than a correctness one, because `mouse:` is the line nothing
reads. `units:`, the roster block and `peek:` each hold their own clock inside `roster_log`, so
they keep their cadences whatever the fill rate does — which is the property the force exists
for, and the one both runs confirm.

**GATED IN MILLISECONDS, NOT IN FRAMES.** The old throttles counted render frames at ~60/s
(`% 300`, `>= 30`, `>= 15`). This runs from the in-play draw instead, and the draw rate is far
higher than 60/s on every reading anyone has taken — **7 388 and 13 361 draws/s** on this
instance across two runs of the same fixture, against the means of **816** and **842** recorded
in [gpu-status](gpu-status.html) under different conditions and the **330..4900** band
`tagpu_packet.c` documents. **Those figures are not reconciled and this landing did not
reconcile them**; what matters here is that the conclusion holds at every one of them — even at
the lowest recorded mean, carrying `>= 15` across would be a ~14× increase in file writes on the
game thread inside an engine call, and that is the fault the review of 10c-1 caught when this
family first moved.

**ONE BURST BOUND, ADDED BY THE REVIEW.** `plog` is one `fopen`/`fprintf`/`fclose` per line, and
the roster dump emits one line per unit with `n_units` bounded only by `TAGPU_PK_MAX_UNITS`
(16384). On the render thread a stall cost a dropped frame; on the game thread it costs sim time,
and the fixture used here carries ~11 units, which does not exercise it (97 dump
lines across the run's nine blocks — they do not divide evenly because the alive count moves
while the scenario's units arrive and die). The dump now takes **one
open for the whole block**. The per-unit cost at a realistic unit count was never measured, and
this bound is why it does not need to be.

**AND THE ROSTER NO LONGER HAS AN OFF SWITCH.** `log_units` sat below `tagpu_overlay.c`'s
`tagpu_overlay.off` early return and below the teardown gate; `roster_log` has neither. For
`tacli` that is an improvement — the harness keeps working with the overlay off — and the
teardown gate is moot now that the walk touches no engine memory and runs on the teardown's own
thread. But it is a change to what that kill switch kills, and the new log has no lever of its
own.

**What 10c does NOT close:** the camera hold, `tacli wheel` and anything else riding the overlay
frame still do not reach `renderer=gdi`. The two die at different points, and the review corrected
this: the **hold** dies because `tagpu_cmd_post` is called only from `tagpu_zoom_frame_end`, so
there is no record to carry it; the **wheel** never gets that far, because `tagpu_zoom_wheel`
returns at `if (!s_live …)` and logs *"zoom: wheel ignored — no zoomed world on screen"*, and
`s_live` is set by the render thread's publish. Same cause — no overlay frame — two mechanisms.
Both are properties of the zoom subsystem, not of the trigger family, and they are landing 11's
business.

**10c-2's SUPERSEDED DESIGN, kept as the record** [written 2026-09-18, replaced the same day]:

* **`tagpu_packet_acquire` must never be called from the game thread**, and not merely because it
  consumes. It records `m->consTid` and logs a *"consumer thread N -> M (thread restarted)"* line
  when it changes; it raises the `"acquire twice in one frame"` violation through `m->inFrame`;
  and it `XCHG`s a slot out of the cell, taking it from the render thread. Any one of those three
  is a corruption of the exchange's own bookkeeping, so the accessor has to be a new one.
* **But the accessor needs no synchronisation at all, because the publisher is already the game
  thread.** All three publish sites are in `tagpu_packet_pub.c`, whose header says the publish
  happens *"on the game thread, inside an engine call, at a site that owns what it reads"* — and
  `before_flip` is the same thread. So "the slot I last published" is an **intra-thread ordering**
  question, not a race: the flip comes after that frame's publish and before the next one, so the
  slot cannot have been reclaimed and rewritten underneath the reader.
* So 10c-2 is a read-only `tagpu_packet_pub_last()` returning the producer's last committed
  record, documented as **valid only on the producer's own thread, and only until its next
  publish** — which is exactly the window `before_flip` sits in. No new lock, no new ordering, and
  nothing the render thread can observe.
* **Resolved rather than left as a caveat** [the review of 10c-1's fix]: `before_flip` observes the
  flip's **entry**, and `0x4C67C0` is the cursor draw **inside** that same flip. So a
  `tagpu_packet_pub_last()` read from `before_flip` returns the **previous** flip's shell packet,
  not this one's. For `do_eye` — which wants `in_game` and the camera eye — one flip of staleness
  is almost certainly fine, but it has to be *stated* rather than discovered: the alternative is to
  call the family from `after_flip` instead, which the same observer already provides.

**The transferable lesson, and it is not "the accessor was hard".** Every bullet above is *true*.
The design failed on a question it never asked: **where is this value consumed?** Four of the five
bullets reason about the producer — the exchange's bookkeeping, the publisher's thread, the
staleness window — and none of them follows `s_eyeHold` forward to `tagpu_input_cmd`, which is one
`grep` away and settles it in a line. A move is not decided by what the code being moved reads; it
is decided by what reads the code being moved.

**The paragraph below is what this replaced, kept because its reasoning is the record.**

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
trigger family from a lane-independent point — the game-thread flip `0x4C63A0` runs on every
renderer (`gui: first flip on thread 652` appears in a gdi boot's log) and is the right host.

**THE HOST IS THE OBSERVER THAT IS ALREADY THERE, NOT A NEW DETOUR — and two earlier sentences
here were wrong about this** [verified read-only 2026-09-18, against the tree]:

* **`0x4C63A0` is already hooked.** `tagpu_gui_hook.c` installs
  `tagpu_detour_observe(FLIP_VA, FLIP_STOLEN, …, before_flip, after_flip)`, and
  `tagpu_packet_pub.c` already records the consequence in a log line it prints at boot: *"the
  shell's only publish point: the flip `0x4C63A0` cannot be observed a second time,
  `tagpu_gui_hook`'s hijacks it"*. So 10c does not install anything. It calls the family from
  `before_flip`, which is a two-line change to a function that already runs exactly where 10c
  needs to be.
* **`before_flip` is already the game thread, already guarded, and already does real work.** It
  latches `s_gameTid` on its first call and returns immediately from any other thread
  (`else if (!on_game_thread()) return 0;`), and below that it drains a cross-thread free queue,
  walks a surface census and publishes. It is not a constrained context — unlike the scenario
  applier's creation pass, whose header says *"No file I/O, no CRT"* — so the family's `.trigger`
  polls are in the same class as work the function already does.
* **`Game_MainLoopTick 0x4969D2` is NOT a candidate, though it looks like the obvious one** — it
  is already used for exactly this shape of deferral (`tagpu_scenario.c`, the creation pass, *"never
  mid-render"*). It fails on the one requirement that defines this family: **it stops outside a
  live game.** `tagpu_scenario.c` says so in as many words about ENDMSN.GUI, and the family's own
  comments are explicit that the menus are the point — peek *"must run at the menus too: switch
  effects land before the first game"*, `tagpu_ui_frame` *"the menus are exactly where it earns
  its keep"*. A host that only runs in-game would silently drop the half of `tacli` that drives
  the shell.
* **The family is SIX functions, not the five surveyed above.** `tagpu_input_frame`
  (`tagpu_input.c`, called only from `tagpu_overlay.c`) was missed, and it is in the same class —
  in-process input injection that the comment says *"must run even at the menus and regardless of
  the overlay's enable state"*. It is also the one with the strongest claim to being moved, since
  injected input on the render thread is the odder of the two places to put it.

**WHAT EACH OF THE SIX ACTUALLY NEEDS FROM THE FRAME PACKET** [verified read-only 2026-09-18,
function by function]. This is the design risk this plan named as "the thread move", made concrete:

* **No GL anywhere.** `tagpu_input.c`, `tagpu_peek.c`, `tagpu_weapons.c`, `tagpu_cat.c`,
  `tagpu_gui_snap.c` and `tagpu_scenario.c` contain **zero** `gl*`/`x_gl*` calls between them. So
  the move cannot strand a GL call on a thread with no context, which was the obvious way for this
  to fail silently.
* **Five of the six use `f` only to throttle themselves.** `tagpu_ui_frame` and `tagpu_cat_frame`
  are exactly `if (f && (f->frame_counter % 5)) return;`; `tagpu_scenario_frame` adds
  `g_frame = f ? f->frame_counter : 0;`. `tagpu_peek_frame` and `tagpu_weapons_frame` take a bare
  `unsigned frame_counter` and never see a packet at all. On the flip these take `s_flips`, which
  `before_flip` already maintains.
* **`tagpu_input_frame` is the hard one, and it is also the one 10c exists for.** It has three real
  dependencies, and — correcting this plan's earlier survey — **it does not null-check `f`**: its
  first statement is `tagpu_shield_frame((HWND)f->hwnd);` with no guard.

  | what it needs | where it comes from today | on the game thread |
  |---|---|---|
  | `f->hwnd` | `g_ddraw.hwnd`, copied into the packet at `render_ogl.c` / `render_vk.c` | read `g_ddraw.hwnd` directly — same global, one hop shorter |
  | `s_frame->game_width` / `game_height`, for mapping mouse tokens | the same `g_ddraw` block that fills every other geometry field | same |
  | `do_eye`'s `f->packet->in_game` and `->eye[]` | the published engine packet | **already a game-thread product** — `tagpu_packet_pub.c`'s header says the publish happens *"on the game thread, inside an engine call, at a site that owns what it reads"* |

  So every field is available, and two of the three are *more* natural there than here.

**AND THE "FIVE ONLY THROTTLE" CLAIM WAS WRONG TOO — IT IS FOUR OF SIX THAT PASS `f` DEEPER**
[brace-matched every function body 2026-09-18, rather than grepping for `f->` as the earlier pass
did]. The throttle is the first line; it is not the only use:

| trigger | passes `f` to | what that needs |
|---|---|---|
| `tagpu_ui_frame` | `write_snapshot(f)` | `frame_counter`, `game_width/height`, `vp_x/y/w/h` — all metadata in the JSON |
| `tagpu_cat_frame` | `write_units(f)`, `write_features(f)` | `frame_counter` — metadata |
| `tagpu_scenario_frame` | `place_camera(f)` | `game_width/height`, and this one is **load-bearing**: the fallback is `640x480` and a wrong value misplaces the camera |
| `tagpu_input_frame` | `do_eye(f)`, `do_keys`, `s_frame` | `hwnd`, `game_width/height`, and `f->packet->in_game` / `->eye[]` |

Every one of those derefs is already null-safe (`f ? f->x : default`), which is why the earlier
grep-level reading survived as long as it did.

**SO 10c IS SMALLER THAN "CHANGE SIX SIGNATURES" — IT IS "BUILD A `TAGPU_FRAME` ON THE GAME
THREAD".** `tagpu.h`'s struct documents each field's origin in its own comment, and almost every
one of them is a `g_ddraw` field the render thread merely copies:
`game_width/height`, `vp_*`, `win_*`, `hwnd`, `hdc`, `bpp`. `frame_counter` becomes `s_flips`,
which `before_flip` already maintains. That is roughly fifteen lines in `tagpu_gui_hook.c`, and it
changes **no signature and no callee** — so nothing downstream has to be re-reviewed for a
signature it did not ask for.

**Two fields cannot be filled there, and only one of them costs anything:**

* `surface_tex` — a GL object id, meaningless off the render thread. **Read by none of the six**
  (checked), so it stays 0.
* `packet` / `packet_prev` — read only by `tagpu_input.c`'s `do_eye`, which null-checks it. There
  is **no non-consuming accessor**: `tagpu_packet_acquire(&prev)` takes the packet, and taking it
  on the game thread would steal it from the render thread's own consumer. So either 10c adds a
  read-only "what is currently published" accessor, or **`tagpu_eye.txt` eye-hold stops working on
  the game-thread path** — an opt-in measurement lever, not a play feature, and the only named
  casualty of the move. Decide it when writing 10c rather than discovering it.

**AND MOVING ONLY THE EASY FIVE WOULD NOT ACHIEVE 10c.** The gate's stated purpose is that
`renderer=gdi` can be driven and measured at all; `tacli` drives the game with **injected input**,
so input is precisely the trigger whose absence makes the lane undrivable. A version of 10c that
moved peek, ui, cat, weapons and scenario-detection and left input on the render thread would
report progress and change nothing about the lane. Written down because the easy five are the
tempting scope.

**AND THIS IS WHY 10c NO LONGER THREATENS 10b'S SAFETY ARGUMENT.** The concern recorded here was
that 10c might add a `tagpu_` call to `render_gdi.c`, which would falsify the *"`render_gdi.c`
contains no `tagpu_` call at all"* fact that 10b's inertness rests on. Hosting in
`tagpu_gui_hook`'s existing flip observer touches `render_gdi.c` **not at all** — the detour is on
the engine's own function, which the gdi lane reaches by itself. The two landings are independent
again. **The design risk that looks biggest is not there**: the trigger functions take a
`const TAGPU_FRAME*` only to throttle themselves — `if (f && (f->frame_counter % 5))` is the whole
of their use of it, in both `tagpu_gui_snap.c:613` and `tagpu_cat.c:306` — and every one
null-checks it EXCEPT `tagpu_input_frame` (see the per-function table below, which corrects
this clause), so a game-thread host can pass its own flip count or `NULL` for the other five. What DOES need thought is that they would move from the render thread to the game
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

## THE CLEAN CUT — the engine stops being a layer [LANDED 2026-09-20]

**There is no lever.** The owner's ruling was *"I want pixel compositing with vulkan to be
permanently disabled, not switch or option in engine to bring it back. The code must be a 100 %
gone. We only compare against the second buffer containing the original software rasterizer
output"*, with one condition — *"as long as the original software rasterized is still reachable in
another buffer/texture so that it can be used for reference"* — and one landing, not four.
`tagpu_purevk.on` existed for a day and is gone with the code it gated. The full write-up and the
numbers are [gpu-status](gpu-status.html) §2.81; what belongs here is what it changes about **this
plan**.

### What was deleted

Three paths carried engine pixels to the frame, and all three are removed at the source:

| what | where it was | what is left |
|---|---|---|
| TA's whole composed frame, the frame's bottom layer | `tagpu_vk_surf_record` + its pipeline, descriptors, sampler, buffers and quad; the fork's own `PASSTHROUGH_VERT`/`PALETTE_FRAG` pair | the upload alone — `tagpu_vk_surf.c` is one phase now and has no `record` |
| the UI layer | `tagpu_vk_gui.c` (2897 lines) + `tagpu_gui_surf.c` (2518) + seven shader programs | nothing. `tagpu_gui_hook.c`'s op capture survives |
| the marker pass's captured 8bpp layer | `TAGPU_MK_TEX_LAYER` + `markown`'s context-base swap | nothing. Every other marker is re-derived from engine STATE |

`tagpu_cursown` went too — four engine call-site redirects that skipped the engine's cursor blit
while ours was on screen. Ours was the composite, so the byte they compare could only ever be 0.

**The UI layer took our own work with it, and that is not an accident of the cut but a property of
how it was built.** `LAY_FS` sampled the engine's replayed twin *and* G17a's device-resolution
sharp layer through ONE quad: the engine's half could not be removed from that draw and ours left
in. What the player has now is no HUD, no sidebar, no minimap, no cursor, no dialogs and no shell.
The UI returns as a pass of ours, built from the op stream the hook still captures — which is the
engine's UI stated *semantically*, and the reason that file survived. (The other reason is that
`before_flip` is the only host of `tagpu_triggers_frame`; deleting it takes every `tacli` verb.)

### What the reference is

`tagpu_surf_take` still captures TA's composed frame on the render thread; `tagpu_vk_surf.c`
still uploads it per frame slot as an R8 index image with its palette beside it, left in
`SHADER_READ_ONLY_OPTIMAL`. `tagpu_vk_surf_engine_view(slot, &w, &h)` hands the view out, with no
sampler — a consumer brings its own, and it must be NEAREST.

**It has no consumer in the tree, deliberately.** The one it had was the composite. The upload
costs one memcpy and one `vkCmdCopyBufferToImage` per *changed* frame — a still screen pays
neither, because the serial gate holds — and the alternative is that the next comparison starts
with a rebuild rather than a call.

### The `*own` levers came off the play defaults, and that is what makes the reference worth keeping

This corrects the published plan's §6, which calls the engine *"a complete, correct reference
implementation of this game's look."* With `terrown`, `featown`, `fxown`, `markown` and `owndraw`
armed it is not: they stop the ENGINE drawing so ours can stand in its place, which was right
while we composited over its frame and now only holes the golden source. Measured before the
change: `TERROWN skip=1 filled=1`, `FEATOWN skip=1`, and a capture holding the HUD over a **flat
key-coloured viewport with no terrain and no trees** — 625 049 px of raw key.

They are not deleted. Arm `tagpu_terrown.on` and the engine's terrain stops exactly as before; the
CPU it saves is real and is the whole of what those levers buy now. It is not the default, because
a golden source with holes in it is the more expensive mistake.

**`tagpu_terr.c`'s emit gate went with the composite.** The pass refused to emit a single cell
without `terrown` installed, and its stated reason was the key test — our terrain is opaque, so
over the engine's frame it would have hidden the bars, wireframes, cursor and chat that the key
test let through. Nothing is under us now. `own` stays and still asks whether to stop the
*engine*; the gather no longer asks anything.

### Measured

feat-forest, Two Continents, `--los 0`, **the play defaults**, three settled grabs each, all three
identical:

| | |
|---|---|
| the presented frame | the engine's chrome — top bar and sidebar, 118 016 px — is the seam's clear colour **exactly**; 0 px of anything else. 0 teal and 0 raw key anywhere in the frame. |
| the golden source | 0 raw key, 0 teal, **170 distinct colours** in the viewport and 59 in the sidebar: terrain, water, trees, units, health bars, minimap, resource bar, cursor. A complete 1997 frame. |

`viol=0 crcbad=0 trunc=0`, no `ErrorLog`, sim ticking 30/s at speed 10. The harness drove the
whole blind shell — SINGLE, Skirmish, Start — with no UI drawn, which was the untested assumption
the handoff put first and is now measured twice.

`1791a7e` is **reverted**. It stole two branches to stop the engine's feature-shadow ALP blend
landing as teal against terrown's key fill — a defect of the composite, measured at 40 861 px.
There is no composite, and with `terrown` off the defaults there is no key fill either. Two engine
byte patches for a picture nobody sees.

### What the cut does NOT do

Stated so the next session does not look for it: it does not stop the engine rasterising (that is
still the last step, and the reference is now the reason as well as the obstacle), it does not
move `tagpu_surf_take` to the game thread, and it draws nothing back.

**`tagpu_surf_take` is still on the render thread and nothing sequences it against the game
thread's draw**, so the capture can land mid-frame and the reference be a torn picture. The hook
it wants exists: the packet publisher's `after_draw` observer on `DrawGameScreen 0x468CF0`
(`tagpu_packet_pub.c`), where the frame is complete on the thread that drew it. **This must be
fixed before anyone trusts a pixel diff against the golden source.**

### The dormant contracts the cut left standing

Three things now have a producer and no consumer. Each is a capability waiting for the native UI,
not an oversight, and each is named here so the next session does not read it as dead code:

* `tagpu_vk_surf_engine_view` — the golden source, above.
* `tagpu_gui_set_want_minimap` / `tagpu_gui_set_minimap_have` — the packet publisher still builds
  the level's 252×252 TNT minimap picture and the three live minimap surfaces, gated on a consumer
  asking. Nothing asks, so nothing is paid for.
* `tagpu_gui_installed` — the module's public "did the observers go in" predicate.

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
