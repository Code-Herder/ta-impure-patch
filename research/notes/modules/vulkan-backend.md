# The Vulkan backend

The lane, the shader pipeline, float depth, lines, hardware portability, vsync and presentation lifetime.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.24 The Vulkan lane (`tagpu_vk.c`, **off unless armed**, `tagpu_vk.on`) — Phase G, G19a + G19b

*(This was numbered 2.20 until 2026-09-15, which §2.20 — landing 4b's fog grids — already was; §2.21c's "the static fixture of §2.20" means that one.)*

**It touches no engine address, and it must never need to.** `tagpu_vk.c` is not on
`thread-split.allow` and every value it uses arrives as an argument from `ogl_render` — which is
Phase G standing constraint 1 ([roadmap](../roadmap.html)) and the line that decides whether the
renderer can ever be lifted into another process. So it has no row in the hook map above.

**Where it sits in the frame.** One call, in `ogl_render` immediately before the GL swap:

```c
if (!tagpu_vk_frame(g_ddraw.hwnd, g_ddraw.render.width, g_ddraw.render.height, g_config.vsync))
    SwapBuffers(g_ogl.hdc);
```

It returns 1 only when Vulkan presented the frame itself, and the GL swap is then skipped — two
backends must not both present in one frame. With `tagpu_vk.on` absent it returns 0 on a cached
lever read (250 ms, as elsewhere in the fork) before touching anything.

**Route D: Vulkan has its own window** — and the other two routes are not fallbacks, they are
broken on system wine. The measurement, the route table and the reason the API's "yes" could not
be trusted are in [roadmap](../roadmap.html) Phase G, *Coexistence*. What the module does with it:
an owned top-level popup over the game window's client rect, `WS_EX_NOACTIVATE |
WS_EX_TOOLWINDOW` so it never takes focus or enters the taskbar, `HTTRANSPARENT` so a click falls
through to the game window, created / moved / destroyed **on the window thread** (posted
`WM_TAGPU_VK`, and `WM_WINDOWPOSCHANGED` on the owner is what keeps it in place — event-driven,
so the render thread polls no geometry). The observer in `wndproc.c` claims no message.

**The five-state machine is the whole of the thread safety**, and it is an ordering rather than a
lock:

| state | who may touch the Vulkan objects |
|---|---|
| `ST_OFF` | nobody; nothing exists |
| `ST_STARTING` | the worker that took the lane, alone (a bring-up, or the enumeration) |
| `ST_READY` | the render thread, alone |
| `ST_FAILED` | nobody; the objects are already gone |
| `ST_ZOMBIE` | **the abandoned worker, alone** — it is still the sole owner of everything it built, which is what lets it put its own objects back and hand the lane on |

(That last row said "nobody" until the second review pass, and believing it is
what put a `DestroyWindow` in `tagpu_vk_render_stop`'s tail underneath a
zombie worker's live surface.)

`OFF → STARTING` is an `InterlockedCompareExchange` taken *before* a worker is created, so two
frames cannot start two workers **and the enumeration worker takes the same grant** — that last
clause is the landing review's, and without it the two workers overlapped and shared `s_mod`,
`s_gipa` and the whole instance-level dispatch table, which belongs to an instance. `STARTING →
READY` is a **compare-exchange** the worker does after every field is written (also the review's:
an unconditional store could stamp `READY` over an `ST_ZOMBIE` set behind its back). `READY → OFF`
is the render thread's alone. So no two threads touch `s_vk` — **and, since the enumeration is
inside the same grant, no two touch the dispatch table either.**

**The bring-up is on a worker thread and that is not a preference.** `vkCreateInstance` loads the
ICD, so it *is* a `LoadLibrary`, and [field notes](../field-notes.html)'s rule — load from your own
thread, never from `DllMain` or mid-present, through `real_LoadLibraryA` — was paid for once
already by the companion-DLL design.

**`ST_ZOMBIE` is a worker winding down, and NOTHING WAITS FOR IT.** An earlier shape had
`tagpu_vk_render_stop` wait five seconds for a worker still in `ST_STARTING`, then abandon and
leak its objects. The review killed both halves. The game thread waits `INFINITE` on the render
thread across a mode change (`dd.c`) and the render thread was waiting on the worker, so a mode
change that caught a bring-up in flight stalled the **lockstep world** for as long as the
bring-up had left — 371–451 ms, routinely — and because the window thread *is* the game thread
here, a winevulkan call that reached the window with an inter-thread send would have closed the
cycle game → render → worker → window with only that timeout to break it. Load-bearing, which the
note claimed it was not.

Now `render_stop` marks the lane `ST_ZOMBIE` and returns at once. `ST_ZOMBIE` means *a worker is
finishing and nobody else may touch anything* — so the worker, which is still the sole owner of
everything it built, puts its own objects back, destroys its own window and hands the lane to
`ST_OFF`. Nothing waits, nothing leaks, and the lane can be brought up again afterwards instead
of staying down for the session.

**The window belongs to the lane while it is up or coming up, and to nobody otherwise.** That is
one function, `vkw_release_unless_worker`, and it exempts exactly `ST_STARTING` and `ST_ZOMBIE`
— the two states in which a worker may hold a surface on the window and takes it down itself.
The rule was spelled out at four call sites with three different guards until the second review
pass, and two of them were wrong: `render_stop` destroyed the window under a zombie worker's
surface, and an armed lane whose bring-up **failed** kept a shown, never-painted popup over the
client area for the rest of the session.

**The residual at `WM_DESTROY` is wider than it was, and this is the statement of it.** While
`render_stop` waited for its worker, stopping the render thread implied the worker was done, so
the handler was a backstop that found nothing up. It no longer implies that: an ordinary
shutdown or mode change can run `DestroyWindow` on the game thread while an abandoned worker is
still putting a surface on the window — and Windows destroys an owned popup with its owner
whatever we do. Acquire then returns `VK_ERROR_SURFACE_LOST_KHR`, which the lane now treats as
fatal, so the outcome is a lane that comes down rather than one that spins. The orders that
would close it are a cross-thread block (the deadlock this module is arranged to avoid) or a
window outliving its owner (which Windows does not allow), so it is named rather than fixed.

**What the lane costs, measured in game** (640×480, 1024×768 and 1920×1080, shell → game →
shell): the Vulkan window tracks the client rect **exactly** (bbox identical to `xwininfo`'s, 2
073 600 of 1920×1080 px magenta), bring-up is **371–451 ms** on the worker, peak committed grows
**+5.3 to +6.5 MB**, and the **largest free VA block does not move at all** (247.4 MB before and
after) — which is the number that matters in a 32-bit process, because TA's allocator fails by
failing rather than by saying anything.

**The levers.**

| file | what |
|---|---|
| `tagpu_vk.on` | arms the lane. Optional token `color=r,g,b` moves the clear colour off the default black (`color=255,0,255` is the bring-up magenta, the "is the lane on screen?" sentinel) |
| `tagpu_vk.off` | turns the WHOLE module off, the GPU enumeration included — the control for an A/B against a pre-G19 DLL, and it beats `.on` |
| `tagpu_vk.gpus` | written by the enumeration worker: one line per device, `<flag> <name>`, where the flag is 1 for `DISCRETE_GPU` and 0 otherwise. The menu reads it at the NEXT attach |
| `impure.cfg` `gpu=` | the player's choice, by NAME so adding or removing a card cannot silently re-point it. The settings store's key ([renderers](../renderers.html) §2.10b); `tagpu_vk.cfg` held it until 2026-09-23 and a first run renames an old one to `.migrated` |

**G19b's row is in §2.12's front-end table.** Two bounds are worth repeating because both were
found by building it. A device name is canonicalised and truncated on the way in — it comes from
the driver and lands inside a generated `.GUI` where a pipe and a semicolon are syntax. And the
list is capped at **eight** devices, which is **ours and not the engine's**: it said four, on
[GUI gadgets](../gui-gadgets.html) 10.2's claim that a stage button cannot carry more stages than
`commongui.stagebuttnN` has art for, and this landing's review disproved both. `0x4A8003` clamps
the art index before the name is built, so a row past four stages draws the four-bar plate and
works — and the shipped `UI scale` row has carried six since G18f. Past the fourth device the
plate's bar count saturates while the caption stays right, which is the half that says which
card. The caption is clipped to the **120×20 plate** whatever width the gadget
carries ([GUI gadgets](../gui-gadgets.html) 10.2), which is why `build_gpu_text` drops the longest
leading run of whole words every listed device shares rather than trying to widen the row.

### 2.25 The shader pipeline (`tools/spirv-gen.py`, `tools/spirv-check.sh`, `tagpu/ddraw/inc/spirv/`) — Phase G, G19c

**The GLSL does not move, and that is the design rather than an economy.** The fork's shaders live
as C string literals inside the pass that owns them, next to the comment that explains the maths.
A Vulkan lane carrying its own edition of them would be a second copy of twenty-one programs that
nothing forces to agree, and Phase G's whole method is that the GL lane is the ORACLE for the
Vulkan one — two shaders that disagree cannot be each other's oracle. So `tools/spirv-gen.py`
reads them back out, transforms them and compiles them:

```
the C string --(cc -E)--> GL 3.3 GLSL --(transform)--> Vulkan GLSL --(glslang)--> SPIR-V
                                                                   --> uint32_t[] C header
```

**Through the C PREPROCESSOR, not a regex.** `tagpu_terr.c`'s vertex shader splices
`TAGPU_EDGE_NUDGE` into the middle of a line, so the extraction has to expand macros exactly as
the compiler does. `$(CC) -E` is what does it — and its `# <line> "<file>"` markers have to be
skipped, because the quoted FILE NAME in one of them is otherwise spliced into the middle of a
shader, which compiles and then draws something wrong.

**The invariant, checked on every run rather than argued.** Strip the global-scope `in` / `out` /
`uniform` declarations out of a shader and out of its translation, and the two must be
**byte-identical** — every function, every constant, every line of every body. That residual is
**75 % of the source text** (27 687 of 36 744 bytes over the thirty-four). The transform may touch
declarations and the two built-in renames below, and the build fails if it ever touches anything
else. Verified not to be vacuous: a one-character edit inside a body is caught.

**The transform, in full.**

| | |
|---|---|
| `#version 330 core` → `#version 450` | |
| non-opaque uniforms → one **unnamed** `std140` block | unnamed, so every reference in the body still reads `uGame` and no body line changes. The block's std140 offsets are computed and written into the generated header beside the code, because the C side has to fill that buffer and the offsets are the contract |
| samplers and named blocks → a `binding` | |
| **bindings are allocated BY STAGE, not per program** | vertex: 0 = the globals block, 1.. named blocks, 8.. samplers; fragment: 32, 33.., 40.. . `QVS` is the vertex stage of six programs, so a per-program allocation would need the same source compiled six times and two programs could then disagree about what binding 3 is. The gap means a vertex and a fragment stage can never collide whatever either declares |
| varyings → explicit locations, **decided by the vertex stage** | each vertex `out` takes locations in declaration order; a fragment `in` of the same name takes the number ITS vertex stage gave it, so a fragment stage that declares a subset, or in another order, still matches. A fragment input with no matching vertex output is an error here rather than a link failure in a driver |
| fragment outputs → locations in declaration order | which is the order `glDrawBuffers` addresses them in under GL |
| `gl_VertexID` / `gl_InstanceID` → `gl_VertexIndex` / `gl_InstanceIndex` | the same value for every draw the fork issues (`firstVertex` and `firstInstance` are 0 everywhere) |

**What it deliberately does NOT do**, because each is pipeline state and a source edit would make
the shader disagree with its own GL oracle: the **Y flip** (a negative viewport height — G19d),
the **depth range** (GL maps clip z [−1, 1] to [0, 1], Vulkan takes [0, 1] directly, and every
shader here already writes a z in [0, 1], so under GL the near half of that range is thrown away
and under Vulkan it is not — a pass that depth-tests must account for it), and **`gl_FragCoord`'s
origin** (lower left under GL, upper left under Vulkan; a flipped viewport puts it back).

**The count: 34 shader sources in 21 programs, not 37.** The roadmap's 37 was a `grep -c '#version
330 core'`, and three of those hits are not shaders — a comment in `tagpu_terr.c`, a comment in
`tagpu_restore_glsl.h`, and the `_snprintf` in `tagpu_restoreglsl.c` that builds a runtime prefix.
Twenty-one programs over thirty-four sources because six of them share `QVS` and two share
`tagpu_native`'s fragment stage. **39 649 SPIR-V words** in eleven headers, 546 KB of text.

**The one cross-module pairing is a real check.** `tagpu_posedraw`'s vertex stage is linked with
`tagpu_native`'s fragment stage (`tagpu_native_unit_fs()`), so that fragment shader's varying
locations are computed from two different vertex stages and the generator fails if they disagree.
They agree.

**Where the translation runs: NOT in the build.** The roadmap's gate asked for build-time
translation and named this as the fallback; it is the better answer rather than a retreat. The
build has four entry points — `tagpu/ddraw/Makefile`, the CI job that runs it, `build.cmd` and the
MSVC project — and only the first two are ours. A GLSL compiler in the build breaks the other two
outright and puts a 30 MB toolchain between a contributor and a DLL, to translate text that
changes when a shader changes, which is to say almost never. So the SPIR-V is generated on a desk
and committed as text (`uint32_t[]` C headers, so `.publish-allow` has nothing to refuse).

**What stops committed generated code from rotting**, which is the only real objection to it.
Every header carries, per shader, **three** hashes: the SHA-256 of the Vulkan GLSL it was compiled
from, the SHA-256 of the **words themselves**, and the hash of the tool's own transform.
`tools/spirv-check.sh` — a prerequisite of `ddraw.dll` in the Makefile, in the same place and for
the same reason as `thread-split-check.sh` — re-runs the extraction and the transform (the C
preprocessor and python3, **never glslang**), compares all three, and then syntax-compiles each
header standalone because most of these arrays are not `#include`d anywhere yet. **2.0 s**, and
both halves are verified: one character changed in a shader fails the build naming that shader,
and so does one word changed inside a committed array.

**The words hash is the landing review's.** The gate covered only the GLSL, so a hand-edited,
truncated or badly-merged array passed `--check`, passed `-fsyntax-only`, and would have shipped.
What is still *not* asserted — and cannot be without the compiler — is that these words are what
glslang would emit today from that GLSL; the compiler is pinned by version and by hash instead.
**python3 is a build dependency of the Makefile** because of this check, and the CI job names it
explicitly rather than relying on the runner image having it.

**Four bounds the generator did not state and now does**, all from the review, none reachable by
today's shaders and not one of which would have announced itself:

| | |
|---|---|
| a census of the `"#version` literals in each preprocessed source against the shaders extracted from it | a shader written in a shape the extractor cannot read is an ERROR, not a silent omission — and it would have escaped the "a shader no program uses" guard as well |
| a refusal past **seven** named uniform blocks in one stage | the eighth would run into that stage's sampler bindings, and two descriptors at one binding is a validation error nobody here runs a layer to see |
| a fragment input must agree with its vertex output in **type, array size and interpolation qualifier**, not only in name | otherwise it is a pipeline-creation failure on a device rather than an error here |
| the brace depth kept honest on the rewriting path | a line carrying both a declaration and a brace would have made the rest of the shader look like global scope |

**glslang is pinned by version AND by hash** in `tools/glslang-vendor.json` (16.6.0), fetched by
`tools/glslang-fetch.sh` into gitignored `tools/glslang/` — a dev-loop tool, the same shape as
`tools/ghidra/` and `tools/vendor/`.

**The restorer's five shaders are NOT among the thirty-four**, and the reason is a fact about them
rather than a shortcut. `tagpu_restore_glsl.h`'s shaders are compiled at run time under a prefix
the DEVICE decides — `NK` from `GL_MAX_UNIFORM_BLOCK_SIZE` and `GL_MAX_DRAW_BUFFERS` divided by
the weights' widest k-block, `WMAX` from `NK` times a number read out of the weights file. `NK`
changes how many fragment outputs the conv pass declares (`#if NK > 1` … `> 4`), so it cannot be a
specialisation constant, and pre-compiling means enumerating a cross product of a device limit and
a data file. That is a decision about what the shipped weights are allowed to be — G19e's, when
the restorer pass is actually ported.

### 2.91 Float depth, the world and the sun-shadow map (`tagpu_vk.c`, `tagpu_vk_shadow.c`, `tagpu_posedraw.c`, `tagpu_shadow_glsl.h`) — G21a

The plan is [hardware portability](../hardware-portability.html) §3 G21a. This section replaces the
24-bit rule of §2.29 ("the format is 24-bit fixed point or there is none"), the X8_D24 fallback and
`zclipok` of §2.83, and the depth clip control of §2.32.

**The world's depth format** (`vk_depth_format`) is `D32_SFLOAT_S8_UINT`, else
`D24_UNORM_S8_UINT`. The Vulkan specification makes every conformant device offer one of the two
as a depth-stencil attachment, so the world passes arm on every GPU; `UNDEFINED` is left for a
device that offers neither, and the passes that test depth then refuse as before. Both carry a
stencil plane, so `stencilok` is 1 wherever there is a format and the units' Classic hard shadow
(§2.83) never loses its mask to the fallback. The seam logs the one it chose, once per device:
`vk: depth format: D32_SFLOAT_S8_UINT (130)` on the reference setup (`D24_UNORM_S8_UINT (129)` on
main). The world target, the seam's own attachment and the passes' `up` lines follow it unchanged.

**Why float draws the same picture** [SOURCE: `tagpu_vk.c` above `vk_depth_format`; checked
numerically]. Every world pass writes a clip z in [0, 1] and the viewport maps it onto [0.5, 1]
(§2.28). A float32 `f` in (0.5, 1] is `j·2⁻²⁴` for an integer `j` in (2²³, 2²⁴], and D24's
conversion takes `f·(2²⁴ − 1) = j − f`, which lies in [`j − 1`, `j − ½`): its two neighbouring
integers are `j − 1` and `j`. Rounding to nearest or toward zero returns `j − 1` for every such
float (checked over all 2²³ of them): one D24 value per float, in the same order. So on such a
device a depth test between two float32 fragment depths gives the same answer on either
attachment: equal keys tie on both and keys a float apart are apart on both. Vulkan's
float-to-normalized conversion lets a device return either neighbour. Whichever it returns, a
larger float never gets a smaller D24 value, so no test reverses; the most such a device could do
is tie two adjacent floats, and the reference setup ties none it draws [INFERRED from the 0 px
below].

**The range is (0.5, 1], not [0.5, 1]**: rounded to nearest, 0.5 and the float above it
(`0.5 + 2⁻²⁴`) convert to the same value, 2²³. No tested depth comes near 0.5. The highest key a
depth-tested draw carries is `airKey + 5` (`encLayer[9]`) and `depthScale` is `airKey + 8`
(`tagpu_native.c`), so every tested clip z is at least `3/depthScale` and every stored depth at
least `0.5 + 1.5/depthScale`: some 28 000 float steps above 0.5 at the 0.25× scale of about 890.
The marker draws that write clip z 0 have the depth test off (the selection rects, which test,
carry the unit pass's keys). The plan expected changes where two keys lie within one
24-bit step, and named the neighbouring flat features of one row (§2.90) as the known class. That
class does not change: the flats that tie in D24 tie as floats, and the flats a step apart are a
step apart in both. **The format changes a pixel only on a rasteriser that computes a D24
target's depth other than as a float32** [INFERRED]; that the reference setup's does not is
[INFERRED from the 0 px below].

**The sun-shadow map** (`tagpu_vk_shadow_format`) prefers `D32_SFLOAT`, then
`X8_D24_UNORM_PACK32`, then `D24_UNORM_S8_UINT`, taking a candidate with the depth-attachment,
sampled and LINEAR-filter bits before one without LINEAR. The consumers' compare samplers and the
unit pass's 1×1 stand-in follow it (§2.32). On the reference setup it is `D32_SFLOAT` with LINEAR:
the unit pass logs `compare sampler LINEAR`. The texel is 4 bytes either way, so the map's size is
unchanged (16 MB a slot at `shadowres=2048`).

**Its projection.** The light matrix fills [−1, 1] in z, and Vulkan clips to [0, w]. Both caster
vertex stages — the heightfield's `VS_H` (`tagpu_shadow_glsl.h`) and the unit pass's
`uDepthPass == 1` path (`tagpu_posedraw.c` `VS`) — now end with
`gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5`. The clip test `0 ≤ z′ ≤ w` is then the
matrix's own `−w ≤ z ≤ w`, and the stored depth is `(z/w + 1)/2`. That is the value
`negativeOneToOne` with `minDepth 0 / maxDepth 1` stored before, and the value `taShadowAt` compares
against (`p.z * 0.5 + 0.5`). So `VK_EXT_depth_clip_control` is no longer asked for: the extension
rung, its retry and its log line are gone from device creation, `TAGPU_VKPASS::zclipok` is gone,
and neither caster pipeline chains `VkPipelineViewportDepthClipControlCreateInfoEXT`. The
other rungs of the extension ladder are untouched.

**The depth bias, re-checked against float** [SOURCE]. Neither caster pipeline has a rasteriser
bias: `depthBiasEnable` is false in both, so a float attachment's exponent-scaled constant bias
does not arise. The bias is the receiver's, in `taShadowAt`: the normal offset
`W + n·uShScale.x·1.5` in world units, and `z − (1 + 2(1 − n·l))·uShScale.x / uShScale.y`, one to
three texels' worth of depth, in the stored [0, 1] units. The remap stores the same value in the
same units as before, and its depth slope is the one `negativeOneToOne` gave, half the matrix's
clip-z slope, so none of those constants moves (old → new: unchanged). What the format could
change is the step under the bias. D32_SFLOAT's step is 2⁻²⁴ in [0.5, 1) and finer below;
X8_D24's is 2⁻²⁴ everywhere. So float is never coarser, and the bias is far above either.
**None of this runs.** Nothing publishes `TAGPU_SHADOWHAND` and `TAGPU_PDHAND.depthOn` has no
producer (§2.83), so neither caster pipeline is built in this build. The remap is checked in the
translated GLSL (`tools/spirv-gen.py --dump`) and by the arithmetic above, not in a picture. The
map's format is exercised: every launch asks for it, for the consumers' samplers and the stand-in.

**MEASURED 2026-09-25 — the A/B against main.** The reference setup's discrete GPU on a private
Xvfb display, 1024 × 768, `ss=2` (world captures 2048 × 1536, 3 145 728 px), `tagpu_defaults.off`.
Main at `c225c3c` against the branch, both clean builds (`.text` 0xDE3E4 and 0xDE124). The forest
runs used two instances in parallel, one taking main then the branch and the other the branch then
main, so each build ran on both; every other fixture took main, branch, main, branch on one
instance. Every capture was taken twice a few seconds apart
(the static check). Classic, then Classic++ armed live in the same launch (the restorer's lines
quiet first), at 1× and 0.877× (`tagpu_zoom.txt`).

**Each world pass alone** (`.ab`: the pass under test drawing, every other pass `.on=off`, against
the black clear). Changed pixels, main against the branch, of 3 145 728:

| fixture | pass | Classic 1× | Classic 0.877× | Classic++ 1× | Classic++ 0.877× | drawn px (Classic 1×) |
|---|---|---|---|---|---|---|
| `feat-forest` | terrain | 0 | 0 | 0 | 0 | 2 522 876 |
| `feat-forest` | features | 0 | 0 | 0 | 0 | 529 096 |
| `feat-forest` | units | 0 | 0 | 0 | 0 | 36 247 |
| `marker-mix` | markers | 0 | 0 | 0 | 0 | 1 080 |

Each 0 holds for all four pairings (run 1 against run 1, run 2 against run 2, and both crossed);
the static check is 0 everywhere and each build's own run-to-run floor is 0. The forest view is
the flat-tie class's own ground: the feature heartbeat counts 118 anchors in the rect, 31 of them
flat. The markers' fixture drew health bars only (the selection's boxes did not show in it).

Two fixtures could not be paused on one frame twice, and read as noise, not as a result:
`fx-rockets` (a live fight, paused once a projectile exists) differs 1 379–13 108 px between the
builds against a floor of 4 167–12 588 within each build, and the units of `shadow-lab` 17 109–
30 866 against 12 609–28 333. An effects-only capture could not show the format in any case: the
effects pass tests depth and writes none, and alone against the clear every fragment passes on
either attachment. Where effects meet the world's depth, the argument above is the whole case.

**The whole frame, presented** (`import -window`, 1024 × 768 = 786 432 px), with terrain,
features, units and the shipped `shadows=hard` — the units' Classic silhouette through the
stencil, the structures' slant: `selbox-facings` at two stops (tanks on grass, whose silhouettes
fall on the grass; a solar collector on water) and `static-terrain` at a light laser tower on
land. 0 px at every stop, both presets, both zooms, for all four pairings; static 0, floor 0.

**The classes.** None: with every repeatable count at 0, there is no change to classify against
the engine's frame, closer or further.

**Frame time and memory** (MEASURED 2026-09-25, the same GPU): 1920 × 1080, `ss=2`, the play arm
set on `feat-forest`, paused, `maxfps` uncapped; `ftime.on`, four reports of 256 frames a preset,
two launches a build taken main, branch, main, branch. Other sessions' game instances shared the
GPU throughout, so each report moves by more than the builds differ:

| preset | main p50 | branch p50 | main p99 | branch p99 |
|---|---|---|---|---|
| Classic | 1.82 ms (0.73–1.91) | 1.77 ms (1.54–1.99) | 4.57 ms (3.50–7.44) | 4.60 ms (3.92–6.00) |
| Classic++ | 2.06 ms (0.41–2.19) | 2.02 ms (1.67–2.29) | 5.15 ms (4.00–6.10) | 4.88 ms (3.77–10.38) |

The median of eight reports, the range in brackets. No change in frame time is measurable.

**The float format costs memory.** The process holds 599 MiB of video memory on the branch
against 478 MiB on main, in every one of the eight readings (`nvidia-smi`, per process): 121 MiB
more at 1920 × 1080. The world target at `ss=2` is 3840 × 2160 in three frame slots, 24.9 M
pixels, plus the seam's attachment at the swapchain's size; 121 MiB over those is about four
more bytes a pixel, so the driver lays `D32_SFLOAT_S8_UINT` out at eight bytes a pixel where
`D24_UNORM_S8_UINT` takes four [INFERRED from the total]. It scales with the target: a 3840 ×
2160 game at `ss=2` would hold about 380 MiB more for the world target alone. Since the two
formats draw the same picture (above), what float buys is the one format on every GPU, and the
price is this memory where D24 exists.

**Not covered.** The sun-shadow map's picture (no producer, above). A device that offers only
`D24_UNORM_S8_UINT`, where nothing changes from main. Any rasteriser but the reference setup's
under the float format; G21d takes the Windows card. The validation layer, still.

**Risky spots for a review.** `vk_depth_format`'s order and the log line; the device-creation
ladder with the depth-clip rung gone (`dexts[3]` and its bound, the pNext chain now carrying only
the line struct); `tagpu_vk_shadow_format`'s two-pass choice and what `*linearOk` means after it;
the remap in both caster vertex stages and the regenerated `tagpu_posedraw.spv.h` and
`tagpu_shadow.spv.h` (the body program shares `VS`, so its words moved too; its A/B above is the
check that its picture did not); `stencilok` now 1 wherever `dfmt` is set.

### 2.93 Every line is DrawLine's walk on the world target — G21b (`tagpu_line.h`, `tagpu_glsl.h`, the marker, effects and unit passes)

**What it is.** Every line the Vulkan lane draws is `DrawLine 0x4CC7AB`'s walk on the LINE GRID
— the world target's own pixels, `ss` to a game pixel — thickened to `w = ss` pixels across its
minor axis, each end a `w` × `w` block, decided in integer arithmetic in the fragment stage. One rule for every preset:
smooth when supersampled, and at `ss = 1` the engine's own line by construction. That covers the
order markers' target and range circles, queued build sites, selection rects, lasers and lightning,
and the nanoframe wire. The owner's decision and the gate's numbers are in
[Hardware portability](../hardware-portability.html), decision 4 and the G21b block; this section is
the map of what implements it.

| where | what |
|---|---|
| `tagpu_line.h` `tagpu_line_px` | the one endpoint rule: line-grid pixel `floor(ss·Z(p))`, `Z(p) = (p − c)·zoom + c`; `p = k + 0.5` where the engine draws that line from integers, the fractional position otherwise. An end outside the caller's bound is not drawn and is counted (`far=` in the pass's log line): `TAGPU_LINE_FAR` (2^29) for a line that is clipped next, `TAGPU_LINE_MAXC` (16383) for the wire |
| `tagpu_line.h` `tagpu_line_rect` | the viewport's clip rect on the line grid: `{ss·vpL, ss·vpT, ss·(vpL + vw) − 1, ss·(vpT + vh) − 1}`, every game pixel's `ss` × `ss` block inside it |
| `tagpu_line.h` `tagpu_line_clip` | DrawLine `0x4BE950`'s clip on the integer line-grid ends, in its order: `0x4BEA20` against that rect (inclusive), then `0x4CC650` against the line grid's surface (`ss` times the game's). Called where the marker (`put_line`) and effects (`emit_line`) records are built; its ends lie on the surface, which is what keeps them inside the fragment test's ±16383. 64-bit products; the loop of the second half is bounded at four passes (never more than one needed); a surface over 16384 is refused |
| `tagpu_line.h` `tagpu_line_centre` | a record's end: the line-grid pixel's centre in game units, `(T + 0.5) / ss`, so every vertex stage keeps its game-frame mapping |
| `tagpu_line.h` `tagpu_line_grid` | `uGrid` = (line-grid w \| `ss` << 16, line-grid h, target w, target h), from the game frame, `ss` and the extent the pass records into; a grid over 16384 or an `ss` out of range gives `ss = w = 0`, which keeps no fragment |
| `tagpu_glsl.h` `TAGPU_GLSL_BAND_FN` | `taBand`: six vertices a record over two triangles, the segment between the end centres widened 2 game px each side and carried 2 past each end; `t` along the segment for per-end attributes |
| `tagpu_glsl.h` `TAGPU_GLSL_LINE_FN` | `taLinePx` (the target pixel's line-grid pixel, `((2t+1)·lw)/(2·tw)`, the identity on the offscreen target), `taLineEnd` (an end's pixel back from its centre, `floor(c·ss)`), `taOnLine` (`0x4CC7AB`'s walk in closed form, unsigned products, thickened to `w`: kept when within the walk's major range widened by the same window, `[lower − w/2, upper − w/2 + w − 1]`, and within `[r − w/2, r − w/2 + w − 1]` across the minor axis of the walk's pixel `r` at the major position clamped to the walk — so each end is a `w` × `w` block, an engine pixel's end exactly its game pixel's block), `taLineKeeps` (the three) |
| `tagpu_mark.c` `LVS`/`LFS`; `tagpu_vk_mark.c` `s_pipeLine`, `s_pipeLineZ` | marker lines as instanced records (`MVST` stride, instance rate); the selection rects are the depth-tested pipeline's; `uGrid` at 32 of the 48-byte fragment block, written at record time |
| `tagpu_line.h` `tagpu_line_nudge` | an end moved by whole game pixels of the ZOOMED frame, `n·ss` line-grid pixels, after `tagpu_line_px` has placed it; re-bounded to `TAGPU_LINE_FAR` after the move. For a line the engine draws a fixed pixel beside another, whose offset belongs to the screen: an end nudged by one and its neighbour differ by exactly `ss` = `w`, so the two bands abut at every zoom, and at 1× `floor(ss·(k + 0.5)) − ss = floor(ss·(k − 0.5))` is the engine's own offset |
| `tagpu_order.c` `draw_build` | `0x438C00`'s eight lines in integers, in its order (exe-reverse-engineering.md). The colour-B lines are the engine's ends; the colour-A lines are those same ends nudged by the engine's `±1` in zoomed game pixels (`tagpu_mark_emit_line`'s `nudge`), where the engine takes it in 1× pixels. Taken in the 1× frame, that pixel is `zoom` screen pixels, and the pair drew `zoom − 1` pixels apart at a zoom-in (3 px at 4×; at a fractional wheel level the gap differs from edge to edge with each end's own rounding) and on one pixel at a zoom-out. MEASURED 2026-09-25 on `marker-mix` with a queued ARMSOLAR, `ss = 2`, 1024×768: the frame at 1× byte-identical to the build before (0 px in the viewport); at 0.5, 0.8, 1.5, 2, 2.37, 3 and 4 the two lines one contiguous run across every edge, where the build before had a gap of 1–3 px from 2× up |
| `tagpu_fx.c` `LVS`/`LFS`; `tagpu_vk_fx.c` | the `LINES` bucket holds one record a line at the bucket stride, drawn `vkCmdDraw(6, n, 0, first)`; `uGrid` at 64 of the 80-byte fragment block, `ss` from the hand-over |
| `tagpu_vk_unit.c` `wire_records`; `tagpu_posedraw.c` `WVS` | the wire posed on the CPU (the body's 16.16 rounding and projection, key + 0.15) into three-vec4 records after the pose words and before the effects runs, each end put on the line grid by `tagpu_line_px`; `WVS` mixes the per-end key, height and world point along `t`; `uGrid` at 256 of `tagpu_native::FS`'s 272-byte block |
| `tagpu_native.c` FS | `vLineOn`/`vLine`: the line test for the wire; 0 from `VS` and `FXVS` |

**The invariant, and why each part holds by construction.** The pixel decision takes only
integers: two line-grid ends decided once on the CPU, the fragment's own integer target pixel,
and the extent the pass records into. The band only has to COVER: a lit pixel lies within 1.21
game px of the segment across it and 1.06 past an end at every `w = ss`, against a band of 2
(`tools/line-band-check.py`: the closed form equals a step-by-step walk plus thickening at
`w` = 1, 2, 3 and 4, on every shape with one end at the origin and the other in a box of 24,
12, 10 and 8 px respectively, and on long lines to ±16383; the least margin found is 0.882 game
px, 0.75 required). Float error in placing the band corners cannot reach that
margin, and the unsigned products cannot overflow inside the bound. A clipped line is inside
that bound by construction — `tagpu_line_clip` returns ends on the surface and refuses a surface
over 16384 — so a segment with one end far off at a deep zoom is clipped and drawn, not refused
whole; only the wire, which is not clipped, is held to ±16383 itself.

**At `ss = 1` it is the engine's line.** The line grid is the game grid, `floor(1·Z(p))` is the
game pixel, the rect is the viewport's, `taLinePx` is the identity, `w = 1` and the thickening
keeps the walk's own pixel only — every step reduces to the game-grid walk with the clip, with
no second path (`tagpu_line.h`, AT ss = 1). Measured 0 px against the game-grid oracle at an
edge view with 6 lines clipped and 6 rejected (hardware-portability.md, G21b).

**The clip.** Every line kind the lane redraws from the engine's DrawLine (selection rect
`0x467A50`, build site `0x438C00`, the target and range circles' chords, the lasers) passes
DrawLine `0x4BE950`, which runs `0x4BEA20` and then `0x4CC7AB` (whose first act is `0x4CC650`).
A line crossing the viewport edge is walked from where `0x4BEA20` moved its end, and that walk
differs from the unclipped one along the whole visible part — on 50 000 random short
edge-crossing lines, ten pixels a line on average. `tagpu_line_clip` reproduces both halves on
the line grid; the rect is `main+0x37E27`'s {128, 32, W − 1, H − 33} with each game pixel widened to its `ss` × `ss` block, {`ss`·128, `ss`·32, `ss`·W − 1, `ss`·(H − 32) − 1}
(`tagpu_line_rect`), held to the surface as `vpwide` holds the engine's. At `ss = 1` that is the
engine's clip exactly; at `ss = 2` it is the same routine on a grid twice as fine, so a clipped
end lands on a target pixel rather than a game pixel. `tools/line-band-check.py` compiles the C
function with the host gcc and checks it against its own transcription of the two routines on
90 000 lines (same verdict and same ends on all; 44 338 moved, 40 527 rejected). The wire is not
clipped: the engine draws it with the polygon edge walk `0x4C0820`, which clips scanlines, so its
visible pixels stay where the unclipped line puts them.

**The device.** No line primitive is drawn, so the device is created without
`VK_EXT_line_rasterization` and without `wideLines`; `lineok`, `wideok`, `maxLineWidth`,
`vkCmdSetLineWidth` and `tagpu_vk_world_scale` are gone, and nothing logs a line refusal. Every
pipeline keeps `lineWidth = 1.0`, which the specification requires without `wideLines`. With
G21a's depth clip control also gone, the device asks for no optional extension but
`VK_KHR_maintenance1`, its retry ladder is that one rung, and the instance no longer asks for
`VK_KHR_get_physical_device_properties2`, which only those two extensions depended on.

**The A/B's line list.** A frame claimed by `tagpu_mark.ab`, `tagpu_fx.ab` or `tagpu_posedraw.ab`
also writes `tagpu_<pass>_lines.txt` — `# <pass> <grid w> <grid h> <w>`, then `ax ay bx by col`
a line, the ends as integer LINE-GRID pixels. The marker and effects lists carry the clip rect as
well (`# <pass> <grid w> <grid h> <w> <L> <T> <R> <B>`, line-grid pixels) and the ends BEFORE
the clip, collected where each record is built (`TAGPU_LINE_AB`); the wire's list is its records
as drawn. `tools/line-oracle.py CAPTURE LINES` compares a capture holding only lines with its own
walk of that list, clipping first — with `line-band-check.py`'s transcription, not the C —
thickening to `w`, and dropping what lies outside the rect, as the viewport scissor does. It
refuses a capture that is not the line grid rather than resampling it.

**Gaps.** The line-only capture the gate used came from a diagnostic build; the shipped build has
no switch for it. The wire's rule is the walk, not the engine's polygon edge walk: they agree on
steep edges and differ on shallow ones, where the engine lights two pixels a scanline and the
walk a full run (hardware-portability.md, G21b). The clip at 8× was measured under the
game-pixel rule the branch had before and not re-run under this one.

### 2.94 The Windows gate: every pass on an AMD GCN card, and the restorer's unsigned limits (`tagpu_restore_core.c`, `tagpu_vk_restore.c`) — G21d

The plan is [hardware portability](../hardware-portability.html) §3 G21d. It ran on the Windows test
setup (an AMD Radeon R9 200-series card on its Windows driver, 1920 × 1080) through a `tacli`
remote instance (§2.92), with main carrying G21a–c, and beside the reference setup on a private
1920 × 1080 Xvfb display. Both ran the same DLL at `ss=2`.

**What the card does now [MEASURED 2026-09-25].** The log names `vk: depth format:
D32_SFLOAT_S8_UINT (130)`; the world target, terrain, features, units (`hard shadows on
(stencil-masked), nanoframe wire on`) and markers come up on it; and with the play arm set on
`feat-forest` the census reads `6 pass(es) drew … (terr=1 feat=1 unit=1 fx=1 mark=1 scaf=0 gui=1
fps=0)`. G21b's device asks for no line extension, and none is logged missing. GPU frame time
(`ftime.on`, paused, `maxfps` uncapped, four reports of 256 frames each): Classic p50 1.68 ms,
p99 1.84 ms; Classic++ p50 2.64 ms, p99 2.66 ms. The Classic++ terrain restore ran 5 062 frames
in 10.6 s of wall time and 7.0 s of GPU time at NK=8.

**The one refusal the run found, and its fix [SOURCE].** Before the fix the log read
`restorevk: uniform block -1 < one k-block (9472)`, and the terrain, feature, unit and effects
lanes each logged that they draw their base atlas: Classic++ restored nothing on that card. The driver reports
`maxUniformBufferRange` as `UINT32_MAX` (the restorer's own line prints it as `4194303 KB`). The
Vulkan backend passed it to `tagpu_rcore_pick_nk` through an `(int)` cast, where it became -1,
less than one k-block. `tagpu_rcore_pick_nk` now takes both limits as `unsigned`, as Vulkan
reports them, and clamps the quotient to `TAGPU_R_MAXNK` before narrowing it; the call passes
them uncast. Below 2³¹ the arithmetic is unchanged, so no device that restored before picks a
different NK. The other device limits read into an `int` are bounded first:
`tagpu_vk_max_storage_range` caps `maxStorageBufferRange` at `INT_MAX`, and
`tagpu_vk_max_image_dim` returns 0 ("no device yet") for a dimension above it. After the fix, the card's log holds no
refusal line.

**Its pictures against the reference setup [MEASURED].** These are the `.ab` world captures of
G21a's fixtures and two of G21b's, at 1× in both presets, one pass at a time, 3840 × 2160 target
pixels. They ran with `--los 0 --mapping 1` on both machines, because a remote test folder's
store carries the player's own skirmish values. Every capture was taken twice, and each
machine's own run-to-run floor is 0 everywhere, so every count below is a difference between
the two GPUs:

| capture | differ (of drawn) | > 8 levels | > 32 | largest |
|---|---|---|---|---|
| terrain, Classic | 0 of 7 282 264 | 0 | 0 | 0 |
| features, Classic | 12 of 926 348 | 0 | 0 | 1 |
| markers (health bars), both presets | 0 of 1 080 | 0 | 0 | 0 |
| terrain, Classic++ | 180 359 (2.5 %) | 474 | 0 | 29 |
| features, Classic++ | 216 876 (23 %) | 89 864 | 4 320 | 64 |
| units, both presets | 1 207 (2.4 %) | 280 | 158 | 240 |

- **Units**: 12 pixels are drawn on one GPU only; the 158 over 32 levels fall in 50 spots of at
  most 33 pixels, single rows and short runs along polygon edges. The two rasterisers round a
  triangle's edge differently. The Classic++ row matches the Classic one because on each machine
  the two presets' unit captures are identical (0 px): the unit pass drew its base atlas. On the
  AMD card its log gives the reason — the restored twin was published for 1.0× anisotropic
  (`classicpp.cfg=aniso=1`, the A/B's setting) after its sampler was built for 4.0×, and a
  sampler is not rebuilt mid-frame; the reference setup's log of that run was not kept.
- **Classic++ terrain** is the same picture: mean 0.03 levels over the drawn pixels, 99th
  percentile 1, differences along texel edges.
- **Classic++ features** hold the one visible difference: on the AMD card the restored trees
  carry a few off-hue specks (dim magenta, red, blue) near trunks and edges. 2 388 pixels
  change hue by more than 32; the mean is 2.2 levels and the 99th percentile 27. The restorer
  itself caused them: the card's driver dropped the render-pass dependencies between its
  passes. §2.95 has the diagnosis and the fix; with it, this capture differs by one level at
  most.
- **The nanoframe ladder** (G21b's wire): 13 of 74 931 pixels are drawn on one GPU only, so the
  line rule draws the same shape on both. Colours differ by up to 180 because the build colour
  pulses with game time and the two games paused at different ticks.
- **Timing, not the GPU**: `shadow-lab`'s units (the dish and turrets stand at other angles),
  `fx-lasers` and `fx-rockets` are live and paused at different ticks. The reference setup's
  rockets had not fired when it paused, so `fx-rockets` has a Windows capture only.
- The effects captures need `fx.on=nomodels`: the effects' 3D models are drawn by the unit pass,
  and a one-pass capture refuses a frame two passes drew into.

**Not covered.** The hard shadows' picture: they are drawn (logged, stencil-masked), but a
one-pass capture has black under them, and there is no presented-frame capture on a remote
instance. The 0.877× zoom, since a remote instance cannot write `tagpu_zoom.txt`. The validation
layer on either machine. (Restored units across the two GPUs are measured in §2.95.)

### 2.95 The restorer's passes and a driver that drops their render-pass dependencies (`tagpu_vk_restore.c` `end_pass`) — G21d

**What was wrong.** On the Windows test setup's AMD card (§2.94), Classic++ damaged restored,
keyed art. The owner saw it in the UI first: the Impure menu's gear icon, colour smears on the
main menu's buttons, and the team colours. The feature pass showed off-hue specks on trees. All
three lanes are keyed, and all three go through the same restorer passes.

**How it was isolated [MEASURED 2026-09-25].** The captures used `restoredump.on` on
`feat-forest`, with Classic++ armed before launch and the game paused. The dumps are byte
dumps of the feature lane's base atlas and its restored twin, taken on both machines.

- **The input is identical.** The base atlas is the same bytes on both machines.
- **The output is not.** The restored twin differs in 2 807 texels (up to 179 levels), in 6 of
  its 24 frames.
- **The damage follows the slot grid.** A per-frame map of the batches (frame → batch → slot)
  puts every damaged frame in a block of its batch's slot grid, mostly the grid's bottom row.
  That row is the part of the full-viewport draw that the GPU rasterises last. Frames in the
  other slots of the same batches are byte-identical to the reference setup's.
- **The block edges are sharp.** In one 37 × 60 frame, rows 31–48 are wrong and every other
  row is exact. A fault inside the network would spread one texel per layer, so the damage
  enters at the last layers.
- **It is a hazard, not arithmetic.** The damaged texels are the same set in every run, but
  their values change: two runs of the same build differ from each other in 544 of them.

Each variant below was run on the card and compared with the reference setup:

| variant on the AMD card | texels unlike the reference setup |
|---|---|
| as shipped (NK 8) | 2 807 |
| NK 4 | 2 461 |
| NK 1 (179 render passes a batch, against NK 8's 25) | 3 605 |
| slicing budget 100 ms (no batch split across frames) | 2 807, the same texels |
| fp16 activations (against the reference setup's fp16 run) | 5 595 |
| `LOAD` instead of `DONT_CARE` on the activation attachments | 2 807 |
| a full pipeline barrier after every restorer render pass | **0** |

On the reference setup, a 0.5 ms budget, which splits every batch across frames, changes
nothing (0 texels).

**The cause and the fix [SOURCE + MEASURED].** Every restorer pass states its ordering as
subpass dependencies (`rp_deps`):

- **incoming:** `EXTERNAL → 0`, from fragment-shader reads, colour writes and transfers, to
  colour writes;
- **outgoing:** `0 → EXTERNAL`, from colour writes, to fragment-shader reads, colour writes and
  transfers.

By the spec, that orders FILL → CONV × depth → OUT → the consumer's sample. The card's driver
does not honour it for these passes.

What sets FILL and CONV apart from the rest of the backend is that their attachments never
change layout. The activations stay `GENERAL` at both ends and inside the subpass, while OUT and
the mip pass transition their attachments. That this is why the driver skips the dependencies
is **[INFERRED]**. Whether the driver honours the backend's other passes,
which do transition, is not isolated either: none of §2.94's captures shows a hazard in them,
and the owner's review of those captures found none.

`end_pass` now ends every render pass in the file (FILL, CONV, OUT and the mip chain). It
records the same ordering again as a `vkCmdPipelineBarrier`:

- **source:** `COLOR_ATTACHMENT_OUTPUT | FRAGMENT_SHADER`, access colour write;
- **destination:** `FRAGMENT_SHADER | COLOR_ATTACHMENT_OUTPUT | TRANSFER`, access shader read,
  colour read and write, and transfer read and write.

A pipeline barrier's scopes are the whole queue in submission order, so the barrier covers:

- the next pass's sample (RAW);
- the next pass's write of the same attachment (WAW);
- a later pass writing what this one sampled (WAR, through the fragment-shader source stage);
- `dump_step`'s copy.

The subpass dependencies stay. They are what the spec requires, and the OUT and mip passes'
layout transitions hang on them. `dst_ready`'s last barrier before a job's first OUT names
OUT's own stage and accesses (colour-attachment output, read and write) as well as the
consumer's sample, so the cleared destination does not rely on OUT's incoming dependency
either.

**With the fix [MEASURED 2026-09-25, the same scenes on both machines].**

- **Features.** The restored twin is byte-identical to the reference setup's at NK 8 and at
  NK 1. The reference setup's own output is unchanged by the barrier (0 texels).
- **The rendered feature pass.** Its `.ab` capture differs from the reference setup's in
  22 672 of 927 200 pixels, by one level at most. Before the fix: 217 004 pixels, 6 252 of them
  past 32 levels, up to 84. The one-level residue is the twin's filtering, which Classic
  features show too (§2.94).
- **Units** (`nanoframe-ladder`, `native.on=all wrecks`, 60 frames restored). The base atlas is
  identical on both machines. The restored twin plus its mip chain (22 MB) differs in 11
  bytes: four texels of level 0 by one level, plus the border copies and mip texels built from
  them. This is float rounding between the two GPUs.
- **GUI.** The UI atlas fills lazily and packs in a different order on each machine, so its
  texels are compared by content. A texel is matched to one in the other atlas whose 25 × 25
  source window — the network's receptive field — has the same indices.
  - Of 99 268 matched texels, 98 730 are identical, 336 differ by one level, and 202 by two to
    six.
  - The bug's damage ran to 216 levels, in blocks.
  - The small residue lies where a matching window can still differ: the frame's rect decides
    the zero padding, and the colour-key fill searches the whole frame **[INFERRED]**.

**The cost.** On the reference setup, the four feature batches of the first restore take
8 slices both before and after the fix. On the AMD card they take 16 slices of the 12 ms budget
(246 ms of wall time). That is paid once per atlas fill, and a settled scene restores nothing.

**Not covered.**
- A before/after of the shell menus' own pictures. The owner's report is the before; the
  after is theirs to look at.
- Other AMD cards and driver versions.
- The validation layer on either machine.

### 2.96 Vsync, and a swapchain rebuild that keeps the passes (`tagpu_vk.c` `vk_resize`, `fps_limiter.c`)

The option and the frame limiter's one cap are [renderers](../renderers.html) §2.10c. The lane's
side:

**The present mode follows the store every frame.** `render_vk.c` hands `tagpu_settings_vsync()`
to `tagpu_vk_frame`, which marks the swapchain for a rebuild when the answer changes. On:
FIFO. Off: IMMEDIATE when the surface offers it, else FIFO. The present modes on offer
[MEASURED]: the reference setup's 4070 under wine FIFO, FIFO_RELAXED, IMMEDIATE and
FIFO_LATEST_READY (roadmap, Phase G); the Windows AMD card IMMEDIATE and FIFO only (2026-09-25,
the machine's own `vulkaninfo` 1.1.106, driver 2.0.91, API 1.1.108, through a 256×256 test
window from a non-interactive session). Neither offers MAILBOX.

**The AMD card's FIFO does not wait for the blank [MEASURED 2026-09-25].** At 59 Hz, uncapped, in
play at 1920 × 1080 with two swapchain images: FIFO presented about 300 frames a second and
IMMEDIATE about 400 (`ftime.on`'s 300-frame reports, timed on the machine itself). That is why
the limiter keeps a backstop above the refresh rate with vsync on; with it, 59.9–60.0 fps.

**A rebuild that changes nothing a pass holds keeps the passes.** `vk_resize` used to drop every
pass and the restorer on any rebuild, because a new swapchain may come back with a different
image count and the passes size their per-slot arrays by `s_pass.slots`. A pass holds no
swapchain object: it is handed a slot and the extent at record time, and builds against
`s_pass.rp`, whose format is the swapchain's. So when the image count, the extent and the format
all come back as they were, `vk_resize` keeps the passes (`passes_down` otherwise). The device
wait before the rebuild means none of it is in use. That covers the vsync toggle, where only the
present mode moves, and an out-of-date surface at the same size. A resize still drops them.

**The UI pass is the one that stays up across `passes_down`**, since its twins are the only copy
of what the shell has drawn. It gives its two restore jobs back first
(`tagpu_vk_gui_restore_drop`, before `tagpu_vk_restore_down`), because the restorer forgets its
job table on the way down and a job pointer kept across it would alias the next pass's.
`tagpu_vk_restore_down` frees and logs any job an owner leaves standing. On Linux a window resize
goes through the fork's mode set, which brings the whole lane down instead, so this path runs on
a live extent change such as a Windows drag. It was measured with a forced rebuild: [GUI
renderer](../gui-renderer.html) §27.4.

Measured: on the AMD card the toggle logs `swapchain rebuilt in place (2 images 1920x1080) - the
passes kept`, and no second `restorevk: terr: job started` follows; on the reference setup, the
owner's toggle off and back on at 3840 × 2160 fullscreen logged the same, both ways. Before, the
toggle restarted the Classic++ restore, 26.6 s of terrain job at 50–52 fps with the unrestored
art on screen. On Xvfb (1024 × 768, Classic++, three images), a window grab is byte-identical before
and after an off and an on toggle.

### 2.107 Presentation follows the window lifetime (`tagpu_window.c`, `render_vk.c`, `tagpu_vk.c`) — 2026-10-04

The engine's logical mode recreation releases DirectDraw at `0x4B5061`, then asks to resize
its surviving HWND at `0x4B55D7`, before the replacement device exists. The old resize policy
lost its owner when `dd_Release` cleared `g_ddraw`; Vulkan also destroyed its device/surface at
every renderer exit. Those produced the borderless desktop flash and restart gap. The engine
map's *DirectDraw recreation and the borderless handoff* establishes the calls and fields;
[fullscreen-handoff](../fullscreen-handoff.html) records the baseline and fixed captures.

| Seam | Mechanism and ownership |
|---|---|
| Engine SetWindowPos / MoveWindow IAT hooks | A property identifies the owned actual window even with no DirectDraw object; engine calls cannot shrink it in that interval. Real proxy mode calls still position it. No new byte patch |
| dd_SetCooperativeLevel / final Release | Register the persistent comctl32 destruction subclass before installing the proxy procedure. Final Release restores its dispatcher, stops and joins the renderer, and resets DirectDraw as before |
| Ready Vulkan renderer exit / replacement | Retain device, surface, swapchain, last image and pass objects only for the same live window lifetime. The existing join transfers sole CPU ownership; existing per-image fences order GPU reuse |
| WM_DESTROY | Publish the departing generation, remove the lifetime ID, join an active renderer, then hard-stop only with no remaining renderer handle and a still-matching bound lifetime. Another window's destruction cannot claim it; HWND recycling gets a new ID |
| Failed or incomplete lane; GPU choice or actual window change | Full cleanup remains; ST_STARTING abandonment does not wait for its worker. Real extent and present-mode changes retain their swapchain rebuild path |

**MEASURED**, Wine on isolated Xvfb, 1280×1024 borderless, logical 1024×768, Classic++, Vsync:
repeated Two Continents shell/game transitions and final Greenhaven entry expose no desktop
pixels and have no handoff window/surface-child recreation. The initial device comes up once,
and actual main-menu Exit tears it down. Fullscreen/windowed toggles rebuild the swapchain.
The real Win32 subclass test passes restoration, scoped teardown, join ordering, thread
ownership, replacement-object/no-renderer cleanup and 256 new lifetime IDs. A deterministic
test of the production observer/predicate reproduces the old delayed-notification failure and
passes with the generation marker. Normal and stock-limits builds pass.

**Still open:** brief blank frames (0.117 s at end of loading in the Greenhaven
capture), live compositor timing and Windows presentation verification. Cold start and failed
or invalidated lanes still initialize a device. No engine/simulation state is written here.
