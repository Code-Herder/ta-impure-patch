# Log — the Vulkan-only plan, landing 11 and the GL strip

Landing records; the plan is [vulkan-only-plan](../vulkan-only-plan.md).

*A frozen record: nothing new is written here. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.64 The screenshot answers one trigger pass late — landing 11-2 of the Vulkan-only plan

`tacli shot` is the harness's engine-surface capture: `ss_take_screenshot(primary)` reads the
fork's own DirectDraw primary and writes a PNG. It is **not** a GL capture and never was — but
until landing 11-2 it was *polled* in `render_ogl.c`'s present loop and nowhere else, so it
answered on the GL lane alone and was measured failing on gdi and Vulkan by 10c-3's review.

**Its host is `tagpu_triggers_frame`, and the interesting part is the clock.** That family runs
from `before_flip`, the **entry** of `FlipOffscreenToPrimary` `0x4C63A0`. The engine's DirectDraw
arm at `0x4C6475` locks the primary, draws the cursor, copies the back buffer onto the primary
with `0x4CBBE0(&ctx, edi, 0, 0)`, restores the cursor background and unlocks, **all inside that
call** (engine map, VERIFIED — re-checked against the disassembly by this landing's review:
`0x4C63A0` opens `sub esp,0xf4`, Lock at `0x4C6509`, cursor `0x4C67C0` at `0x4C6544`, copy at
`0x4C6553`, restore `0x4C6B70` at `0x4C6585`, Unlock at `0x4C6595`). So at the poll the primary
holds the frame the **previous** flip presented.

**The family therefore services a pending arm before it polls for a new one.** A trigger seen on
pass P is answered on pass P+1, with a frame the engine presented in between:

| half | order within `tagpu_triggers_frame` | what it does |
|---|---|---|
| `ss_shot_service(g_ddraw.primary)` | first | if armed, clears the flag and captures |
| `ss_shot_arm()` | second | deletes `tagpu_shot.trigger`, sets the flag |

Both calls are the same detour on the game thread, one flip apart, so the flag needs no interlock
and no fence — the ordering claimed is the engine's own copy, not a window. Latency is one pass
of the family, i.e. at most its `TRIG_MS` 16 ms gate.

**Two other hosts were tried and both are worse. This is the part worth keeping**, because both
looked correct:

* **`dds_Unlock`'s `DDSCAPS_PRIMARYSURFACE` branch** — shipped for one commit and reverted by the
  second round of this landing's review. It is after the copy, which is what it was chosen for,
  but the branch is gated on `g_ddraw.render.run`, and the **window thread** clears that on
  deactivate, minimise, a fullscreen toggle and a mode change (`wndproc.c`, `utils.c`, `dd.c`).
  It is also reached only when the flip takes its DirectDraw arm; the GDI BitBlt arm at
  `0x4C63C0` never enters it. An arm could therefore wait for an arbitrary later frame and then
  answer with an unrelated picture stamped with the wrong time, or wait for ever. The safety
  argument had reduced to *the next unlock that happens to pass a flag another thread owns* —
  which is the house rule's definition of a fix by timing.
* **`after_flip`** — rejected, and the *reason first written here was wrong*. It is not that
  `after_flip` tests `s_opsLive`; it contains no such test. The gate is the detour mechanism:
  `tagpu_detour_observe` emits `test eax,eax; jz` and only overwrites the return slot when
  `before_flip` returns **non-zero**, and `before_flip` returns 0 at `if (!s_opsLive)`. So
  `after_flip` does not run at all without `tagpu_gui.on` — the conclusion stands, by a different
  mechanism than the one this section claimed for one commit.

**Measured** after the move: a correct main-menu PNG on `renderer=gdi` (136 215 B, cursor
present, which is itself evidence the capture is after the engine's cursor draw) and on
`renderer=vulkan` (136 143 B), a lane where the verb had never worked. Two `tacli shot`s inside
one wall-clock second now produce two files — `ss_take_screenshot` disambiguates with a `_2`
suffix instead of returning FALSE, which used to consume the arm and time the verb out with no
error anywhere.

**Not measured**: a screen that presents only once. A 120-shot sweep through a `scenario load`
returned 71 PNGs and 9 distinct frames and never caught the loading screen, so whether this verb
can reach a single-present screen is open.

### 2.65 The world passes stop drawing — landing 11-3 of the Vulkan-only plan

Landing 11-2 deleted the OpenGL lane. That left `tagpu_overlay_draw` with exactly one caller,
`render_vk.c:232`, inside a loop that begins **after** `tagpu_vk_own_present()` at
`render_vk.c:145` sets the latch — so `gl_draws = !tagpu_vk_owns_present()` is false at every
surviving site and the GL draw halves were unreachable **by an ordering** rather than by
inspection. This landing deletes them. Because they could not run, deleting them changes no
behaviour, and that is the whole reason the lane went first.

| file | what came out |
|---|---|
| `tagpu_native.c` | the unit pass's GL draw, 1 054 lines; the two model-AABB caches (`s_aabb`, `s_sbox`, `aabb_walk`, the `MAABB` type); the whole `tagpu_selgeom.on` apparatus |
| `tagpu_terr.c` | the program, eighteen uniforms, six texture units, the instance upload, one `glDrawArraysInstanced` |
| `tagpu_feat.c` | the program, the uniforms, the shadow pass (depth writes off) and the body pass (on) |
| `tagpu_fx.c` | the program, the uniforms, the four layer draws |
| `tagpu_posedraw.c` | three blocks — `pd_begin`'s program and uniforms, the per-unit uniforms and draw, `_end`'s `glBindVertexArray(0)` |
| `tagpu_render3do.c` | the shade-LUT texture upload and creation, and `tagpu_r3d_lut_texref` |

**Every pass keeps its gather and its hand-over.** The gathers still read engine memory and build
the instances, vertices, counts and texels; the `*_publish` / `*_handover` pairs still carry them
to the Vulkan twins, which is what draws. Verified by running it: `vk: census: 6 pass(es) drew
(terr=1 feat=1 unit=1 fx=1 mark=1 gui=1)` at `ss=2`, with every `refused=`, `viol=` and
`foreign=` counter at 0.

**Three guards lost a term, and the identity is why.** `tagpu_terr.c`, `tagpu_feat.c` and
`tagpu_fx.c` all read `if ((gl_draws && s_state != 1) || total == 0)`. With `gl_draws` false the
first disjunct is false whatever `s_state` holds, so the guard **is** `total == 0` — the gather's
own count, which is what it always really was. `s_state` is separately confirmed to stay 0 here:
each pass calls `init_gl()` only inside `if (!tagpu_vk_owns_present())`, so the program is never
built on this lane and `s_state = 1` is never reached.

**The one part that is not a deletion: the build ghost.** `tagpu_ghost.on` is a play default, and
11-2 moved the default lane to Vulkan, which dropped the ghost with the GL unit pass — named at
the time rather than discovered later. `ghost_record()` is `ghost_pass` minus
`ghost_bind_textures`, minus both `x_glDepthMask` and minus the `tagpu_posedraw_live()`
prerequisite. Dropping that predicate is sound because it reads `s_state == 1 &&
!tagpu_vk_owns_present()` — "the GL posed program is live", false on the only lane there is;
the half that survives is the one still true, that the view must belong to **this** frame. It is
called from inside the Vulkan hand-over branch, after the posed units' window, because ghosts
blend with what they sit on and were drawn last on GL too. **Measured on `renderer=vulkan`**:
`ghost: drawn=2405` with the placement cursor live, and a window grab shows the translucent solar
collector inside the placement square. **Not measured**: the queued-site half, which rides the
same `ghost_one` path but stayed at `queue=0` in every run.

**`tagpu_selgeom.on` is gone rather than left inert.** Its only reader was `selAt1x` inside the
deleted draw, so arming the file would have done nothing while still looking available — a lever
that lies.

**AND THE SELECTION RECT ITSELF IS NOT OURS ON THIS LANE. This landing's review caught the plan,
this page, `ui-markers.md`, the `ta-drive` skill and three code comments all saying otherwise**,
so it is worth stating flatly:

* The rect was emitted **only** inside the GL unit draw, below the `!gl_draws` hand-over return.
  No `tagpu_vk_*` file contains selection-rect code.
* `s_selComplete` — the flag `tagpu_markown.c` reads through
  `tagpu_native_selbox_complete()` to suppress the **engine's** own box draw — had its only store
  in that same block. It has therefore been permanently 0 on this lane since landing 4b, so the
  suppression never fires and **the engine draws every selection box itself**.
* The engine draws them at its **unzoomed** projection, which at zoom ≠ 1 is the visible scatter
  the GL pass existed to avoid.

**None of that is a regression from this landing** — the store was already unreachable and
deleting it changed nothing. It is a gap that has been open since 4b and was being described
backwards.

**And it is not currently VISIBLE, for a reason worth writing down: the scatter needs zoom ≠ 1,
and zoom ≠ 1 is itself unreachable on this lane.** Measured 2026-09-18 by trying it: `tacli wheel`
delivers the command (`input: keys wheel:-3`) and `tagpu_zoom.c` answers `zoom: wheel ignored — no
zoomed world on screen`, because `s_live` is set only where the GL zoom path ran; the packet's
`z=` stays `1.000`. At zoom 1 the engine's box is in the right place, so today the engine simply
draws a correct-looking selection box and nothing looks wrong. **The deviation is latent, not
absent** — it appears the moment the zoom lever reaches this lane, which is a separate known gap
in the same plan. Whoever closes that one inherits this one on the same day.

The seam for a fix is left in place deliberately: a Vulkan pass that emits the boxes writes
`s_selComplete` and the engine stands down exactly as it did on GL.

**[Closed by §2.84, and not through that seam.]** The paragraph above was written before §2.81:
since the clean cut the engine's box lands in the golden source only, so "the engine draws every
box itself" meant no box on the screen at all, at any zoom, and the latent deviation was a real
one. §2.84 draws the rect in the marker pass and leaves `s_selComplete` at 0 on purpose — the
engine's box in the reference is what the new rect is measured against.

The GL path's own numbers — 1 320 differing pixels at `ss=2` for `selgeom main` against the
engine's Bresenham rect — are kept in [UI markers](../ui-markers.html) as the record of that path,
not as the cost of this one.

**Two of the eight files the plan names are NOT in this landing.** `tagpu_shadow.c` and
`tagpu_hires_draw.c` are the ones where the producer *is* the half being deleted: `s_pubHave = 1`
is set only in `tagpu_shadow_end` and `s_hiHave = 1` only in `tagpu_hires_depth`, and both were
called only *below* `tagpu_native.c`'s `!gl_draws` hand-over return. They have been unreachable
on this lane since landing 4b, so deleting them would change nothing today — but it would make
`tagpu_vk_shadow.c` and the caster half of `tagpu_vk_hires.c` unreachable by construction, and
the absence of Classic++ soft shadows and replacement-mesh casters permanent until a Vulkan-side
producer exists. That is a decision about the program rather than about dead lines, so it is the
owner's; the plan's landing 11-3 entry has both readings.

### 2.66 A GL-free pass still owns its GLSL — landing 11-4a of the Vulkan-only plan

`tagpu_mark.c` is the marker layer: health bars, the build cursor, the band box, the order
markers and the group digits. Landing 11-4a removed its GL draw half — `getgl`, `mksh`,
`init_gl`, `upload_layer`, the five `x_gl*` entry points, the program/VAO/VBO/texture statics
and the eight uniform locations — 222 lines out. The gather is untouched: `mk_push` and
`mk_draw` still record every bucket and `tagpu_mark_handover` still publishes them to
`tagpu_vk_mark.c`, which draws them. `tagpu_mark_glreset` stays on the context-lost cascade
because `tagpu_text.c` still holds a GL object; this pass no longer holds one of its own.

**The fact worth carrying out of it: the per-pass GLSL strings are a BUILD INPUT, not dead
code.** Each ported pass's `VS` and `FS` are the source of truth for its *Vulkan* shader.
`tools/spirv-gen.py` reads them out of the preprocessed translation unit, translates them, and
the committed `inc/spirv/<pass>.spv.h` is what `tagpu_vk_<pass>.c` includes and draws with.
Deleting them alongside the GL compile helpers — which is what "remove the GL half" reads as —
retargets the Vulkan pass's shader. It did not get that far only because the build refuses:

```
spirv-gen: the manifest names tagpu_mark::VS and the source does not have it
```

`tools/spirv-check.sh` is an order-only prerequisite of the LINK (`$(TARGET): $(OBJS) |
thread-split spirv`, with `spirv` `.PHONY`), so every build that produces `ddraw.dll` runs it,
including an incremental one that recompiles a single object. It is the link that carries the
guarantee, not the object: `make src/tagpu_mark.o` goes through the implicit `%.o: %.c` rule,
never considers `$(TARGET)`, and checks nothing — which is worth knowing when spot-compiling
one file to chase a warning, as 11-4a did. **A pass can be
entirely GL-free and still have to keep two `static const char*` at file scope forever** — they
are unreferenced by C and live as a build input, which is a distinction no search for callers
can see. With no GL consumer they warn as `-Wunused-variable`, and the silencer is
position-sensitive: `_DECL` is applied with `re.search` on the stripped line
(`spirv-gen.py:532`), so `__attribute__((unused))` works as a **prefix** and is skipped as an
infix, either after the name or before it. A wrong placement fails loudly — the manifest
reports the shader missing — so neither form can ship silently. 11-4a used a local
`#pragma GCC diagnostic ignored "-Wunused-variable"` around the pair, for the comment it lets
you attach rather than because the attribute cannot work.

**And the dead-flag trap this pass walked into is waiting in four more files.** 11-4a first
kept `s_state`, the pass's GL readiness latch, on the reasoning that `tagpu_mark_glreset` still
clears it; the review showed it had become write-only, since nothing could set it to 1 once
`init_gl` was gone. File-scope statics get no `-Wunused-but-set-variable`, so the build is
silent. `tagpu_terr.c`, `tagpu_feat.c`, `tagpu_fx.c` and `tagpu_posedraw.c` still carry theirs
(§2.65 records that each is separately confirmed to stay 0 on this lane), and they become the
same write-only flag the moment their `init_gl` goes. Delete the flag with the thing that
raised it: re-introducing `if (s_state != 1) return;` over a flag nothing raises returns on
every frame and publishes nothing, which reads as a pass that was never armed.

**Measured, and the method is the part worth reusing.** `scenarios/selbox-slope.json` pins the
camera over three held Stumpys, so the presented frame is static but for one pixel. Over **8
grabs of a single build** the animation set — every pixel differing between any two of them — is
exactly **1 px at (512, 384)**, the window centre where the pointer rests: TA's own cursor,
pulsing between `[11,11,0]` and `[251,251,251]`. It is in the frame, not in X's overlay, so
`ffmpeg -draw_mouse 0` does not remove it. Excluding it, **all 16 cross-build pairs** (2 grabs
of `315e496` x 8 of this landing) differ by **0 px of 786 432**. `mark: bars=3` on both builds,
`census: 6 pass(es) drew (terr=1 feat=1 unit=1 fx=1 mark=1 scaf=0 gui=1 fps=0)`, three green
health bars on screen.

**This landing first reported a 0-px noise floor from a single pair, and that was phase luck.**
The post-review rebuild re-measured at **1 px, max channel delta 251** — a delta far too large
to be rounding, which is what forced the eight-grab characterisation that found the cursor. The
rule for any cross-build frame diff here: **two grabs are not a noise floor for anything that
pulses.** Take enough grabs to cover the period, publish the varying SET, and diff outside it.

**What the fixture did not exercise.** Only the bars bucket produced records:
`cursor=0 ordtri=0 ordline=0 text=0(lab=0)`, on the HEAD build as much as on this one, so it is
the fixture and not the change. Every write to those counters is in `tagpu_mark_emit_line` /
`_emit_tri` / `_emit_text`, none of which appears in the diff — they are filled upstream of
everything removed. **And the per-pass `.ab` lever cannot certify one pass here by design**: a
Vulkan frame carries every armed pass at once, so `vk: 1 A/B levers claimed this frame and 6
passes drew into it - nothing captured`. Standing five passes down would change the frame the
capture is meant to certify; the full-window cross-build diff answers the same question.

### 2.67 The UI layer stops drawing — landing 11-4b of the Vulkan-only plan

`tagpu_gui_surf.c` is the GUI layer: the engine's own surfaces mirrored into "twins", the sprite
/ string / fill / outline / copy ops recorded against them, the sharp device-resolution layer,
the cursor and the minimap. Landing 11-4b removed its OpenGL half — **796 lines out, 99 in, the
file 3 179 -> 2 457**. Nine whole-function stand-downs go with their call sites (`twin_colour`,
`twin_col_drop`, `twin_upload`, `twin_clear`, `twin_fill`, `twin_outline`, `restore_step`,
`upload_palette`, `unbind_all`), every lane-gated block with them, and behind those `init_gl`
(68 GL calls), `mksh`, `mkprog`, `quad`, `getgl`, `sharp_drop`, 13 `PFN_*` typedefs, 13 `x_gl*`
pointers and ~30 `s_u*` uniform locations. The mirror op stream — what `tagpu_vk_gui.c` actually
draws — is untouched.

**The lane premise was established, not assumed, and it is an ORDERING.** Every gated site in the
file is inside a `static`, and a call-graph walk from all nine exports shows only
`tagpu_gui_present` reaches any of them. That export has exactly one caller
(`tagpu_overlay.c:408`, inside `tagpu_overlay_draw`), which has exactly one caller
(`render_vk.c:232`), inside the frame loop that `render_vk.c:145` opens by calling
`tagpu_vk_own_present()` — `InterlockedExchange(&s_ownWin, 1)` — once, before the loop. So
`tagpu_vk_owns_present()` is true at all 30 sites by construction. `renderer=gdi` reaches
`tagpu_overlay_draw` never and therefore never reaches this file.

**Measured**: `census: … gui=1` on every sampled frame, `tacli ui` answers
(`gui ARMMAIN2.GUI 1024x768`), and the cross-build window diff is **0 px of 786 432 across all
64 pairs**, outside §2.66's one-pixel cursor animation set.

**The review found the one thing the build could not, and the reason generalises.** The nine
shader strings are bracketed by `#pragma GCC diagnostic push` / `pop`, and the `pop` landed
**inside a `/* … */` block**, where it is comment text rather than a directive — so
`-Wunused-variable` stayed off for the remaining ~2 200 lines and the "zero warnings" build was
partly an artifact. It had already hidden two of this landing's own leftovers (a dead
`float v[24]` and an unused `gh`). Worse, the first fix repeated the bug: placing the `pop`
after `LAY_FS`'s terminator with a plain regex stopped at a `;` that ends a line *inside* that
same comment. **The fix is to find the terminator with comments and string literals masked, and
then to PROVE the bracket** — plant an unused static immediately after the `pop` and confirm the
compiler reports it. Reading the file says nothing: both broken placements look right. Landing
11-4a's bracket was probed the same way afterwards and is sound.

**And an instrument that could no longer move.** `s_clears` was incremented inside `twin_clear`,
below that function's lane gate, so it already read 0 — the diff looked safe because the
*reading* did not change. But the landing deleted the only statement that could ever move it,
while `case PK_CLEAR` keeps draining and keeps emitting `TAGPU_GUIOP_CLEAR`. The counter is now
incremented in the drain beside `mir_op()`, where `s_bars`/`s_rects`/`s_pixels`/`s_seeds`
already are, and the heartbeat went from a permanent `clears=0` to **`clears=52351`** in one
run — fifty thousand mirrored ops the instrument was not reporting. The sibling counters that
are *also* pinned at 0 (`col=`, `rearms=`, `rgb=`, `colvalid=`) were left alone, because for
those 0 is the true answer on this lane. **A counter pinned at 0 beside live traffic is a bug;
a counter pinned at 0 beside no traffic is a fact.**

**Nineteen write-only statics went too** — the six programs, the VAO/VBO, the palette texture,
the sharp layer's texture and FBO, the minimap's pair, `s_gl`, `s_palUpValid` and
`s_sharpFailed` — all assigned only `= 0` in `tagpu_gui_glreset` and read nowhere, which `-Wall`
cannot see because it does not warn for a file-scope static that is merely assigned. With those
gone the file has **no GL dependency at all** (no `gl*` call, no `GL*` type, no `GL_*` macro, no
`oglu_*`), so its `opengl_utils.h` include went as well — **the first of that header's nineteen
includers to leave it.** The other eighteen still carry 5 to 44 `gl*` calls each, which is the
real shape of 11-5: the header cannot be deleted until their GL bring-up is. The GUI *is* the side panels and the
top bar, so the whole-window diff tests this pass directly.

**The dead-state rule from §2.66, applied and then applied against itself.** With the GL ids
uncreated, `twin_sprite` and `twin_copy` returned record bytes over `t->rgb`, `src->rgb` and
`s_colValid` that could no longer be anything but 0 — which is what landing 4b-3 already said
this lane records ("indexed"). Folding them to a literal 0 with the reasoning kept is the same
call as deleting `s_state`: a value computed from flags nothing can raise reads like a live
decision. `TWIN` loses `tex`/`fbo`/`rgb`, keeping the `surf`/`w`/`h` bookkeeping `twin_find` and
the mirror ops need, and `twin_drop`'s three `glDelete*` calls go with them. **`s_colValid` was
kept**, pinned at 0 with the reason on its declaration — and the distinction is the useful part:
`s_state` was a READINESS GATE, where a re-introduced `if (!ready) return;` silently publishes
nothing; `s_colValid` is a VALUE that is published as `s_mHand.colourTwins`, where 0 is the
correct answer. Deleting it would reshape the hand-over — a protocol change for
`tagpu_vk_gui.c`, not a deletion. **Dead state that feeds a guard is a trap; dead state that
feeds a published value is a constant, and the two are not removed the same way.**

### 2.68 The support passes stop drawing — landing 11-4c of the Vulkan-only plan

`tagpu_fps.c` (the frame-rate overlay), `tagpu_scaffold.c` (the G12a blocking-map overlay),
`tagpu_gaf.c` (the sprite atlas) and `tagpu_posebake.c` (pose baking) lose their GL halves —
`init_gl`, `getgl`, `mksh`, `serr`, `flog`, the `PFN_*`/`x_gl*` blocks, the uniform locations,
and `tagpu_fps.c`'s `s_state`, which was §2.66's trap again: nothing could raise it once
`init_gl` went. `tagpu_scaffold.c` also loses **`tagpu_scaffold_texref`**, an exported accessor
returning its GL texture id that had no caller anywhere in the tree even before the landing.
Both shader-carrying files keep their pair behind a pragma whose `pop` was probed, per §2.67.

**Two of the four are not the uniform case, and that is the transferable part.**

`tagpu_gaf.c` is **reachable on this lane** — `tagpu_gaf_atlas_create` has six call sites in
five files, one of them `tagpu_gui_surf.c`'s `atlas_setup`, which §2.67 deliberately kept
alive. Every other file in 11-4 was unreachable; this one is not. It survives the deletion for
a different reason: landing 4b-2 had already made the GL texture name optional there, so the
`glGenTextures`/`glTexImage2D` block goes and the shelf packer, entry table and CPU mirror
stay. Likewise in `atlas_paint`, where the `glTexSubImage2D` upload goes and **the `memcpy`
into `a->mirror` beside it stays** — that mirror is what the Vulkan passes sample. Deleting
this file's GL for "it is unreachable" would have produced the right diff from a false premise.

`tagpu_posebake.c` carries a site that is **not a draw gate at all**:
`if (!s_mirrorWant && tagpu_vk_owns_present())` is the POSITIVE form — it ARMS the pose mirror
*for* the Vulkan lane. It simplifies to `if (!s_mirrorWant)`; deleting it would have removed
the lane's own bring-up. **Not every `tagpu_vk_owns_present()` is a GL gate**, and a sweep that
greps for the call rather than reading its sense will get this exactly backwards.

**Measured**: 0 px of 786 432 across all 64 cross-build pairs, with the atlas healthy
(`feat: … atlas=24`, `fx: … atlas=10`; a broken GAF mirror shows as missing sprites
everywhere). The default fixture leaves `scaf=0 fps=0`, so both were armed to exercise them:
the census goes to `7 pass(es) drew … scaf=1 … fps=1`, and a grab shows `FPS59` top-left and
the scaffold's blue→red ramp over the blocked cells — its fragment shader's exact gradient.

**Observed, and attributed to something older than this landing.** With the scaffold armed the
census also reports `unit=0` and the tank bodies are absent while their health bars remain;
disarming returns `unit=1`. The scaffold is an occlusion input to the unit shader
(`tagpu_native.c:508`), and `tagpu_native.c` is untouched by this landing.

The landing's review closed the rest **by construction**: every construct removed from
`tagpu_scaffold_frame` is inside `if (gl_draws)`, or is `if (gl_draws && s_state == 2) return;`,
or is the `gl_draws` declaration — and `gl_draws` is 0 here by the ordering, so deleting a
never-entered block and a never-taken early return is an exact transformation. The rest of the
file hashes identical to the pre-landing revision (`tagpu_scaffold_overlay`,
`tagpu_scaffold_frameinfo`, `stamp_gaf`, `stamp_px`, extracted with a masked brace matcher).
`unit=0` therefore reproduces before the landing too.

**And what the 64 pairs did not cover:** the fixture leaves `scaf=0 fps=0`, so the two passes
this landing changed most put no pixels into the only pixel-exact measurement. The armed run was
eyeballed, not diffed. A regression confined to the armed path would have passed every gate.

### 2.69 The feature and effects passes lose their bring-up — landing 11-5a

`tagpu_feat.c` and `tagpu_fx.c` had no GL draw left (11-3 took both) and no `x_gl*` CALL SITE at
all — only the bring-up that resolved entry points and built a program nothing drew with. Both
files are now GL-free: no call, no type, no constant, no `opengl_utils.h` include. Gone from
each: `init_gl`, `mksh`, `getgl`, the `PFN_*` typedefs and `x_gl*` pointers (six and seven),
`s_state`, the program/VAO/VBO names and every uniform location; and from `tagpu_fx.c` also
`s_lhtTex` and the gated `glTexImage2D` behind it, leaving `s_lhtRGB` as the flash light table
outright — `s_pub.lht` is what the twin builds its image from.

**Both gates are rewritten into the POSITIVE form**, which is the shape 11-4c established.
`if (!tagpu_vk_owns_present()) { init_gl(); … } else if (!s_atlas.made) { atlas_setup(); … }`
becomes `if (!s_atlas.made) { atlas_setup(); … }`. The else-branch was always the one taken on
this lane, so this is exact rather than nearly exact.

**Fields we write: unchanged.** Nothing in the hand-over moved. The one field whose PRODUCER
changed shape is `s_pub.lht` in `tagpu_fx.c`, and only in that the GL texture beside it is gone;
the bytes, the buffer and the serial are the same.

**How it was verified, and why not by an A/B.** Function inventory against HEAD first — exactly
`getgl`, `init_gl` and `mksh` removed from each, nothing added. Then the objects against `main`,
per function: 7 of 11 in feat and 20 of 24 in fx are instruction-identical; `init_gl` had been
INLINED into each gather, which is the whole of the shrink; each `glreset` differs by exactly one
removed store to the deleted `s_state`. **External references lose 22 in feat and 28 in fx — 19
and 25 `gl*` entry points, plus in each case `getgl`'s GetModuleHandleA, GetProcAddress and
xwglGetProcAddress — and gain nothing.** (Undefined-symbol counts 61 → 39 and 67 → 39. The
three non-`gl*` names are INSIDE those totals, not on top of them; the first wording of this
line read as 25 and 31, which would have told a later audit that three references had been
silently re-added.) The include removal was proved separately, byte-identical, because an include is the
one edit that can change a macro silently.

Both VS/FS pairs are kept behind a `-Wunused-variable` pragma and **both brackets were probed
with a planted unused static**, not read: the probe warns after the `pop`, so the suppression
genuinely ends there. `spirv: 49 shaders, 33 programs, headers current` throughout.

### 2.70 The unit pass loses its bring-up, and a gate comes unpinned — landing 11-5b

`tagpu_native.c`, the largest file in the fork, is now GL-free: 367 lines out, no call, no
type, no constant, no `opengl_utils.h`. Gone: `init_gl` (112 of the file's 139 GL call sites),
`tex2d`, `mksh`, `getgl`, seventeen `PFN_*` typedefs and seventeen `x_gl*` pointers, `s_state`,
**fourteen GL object names** (`s_prog/s_vao/s_vbo/s_fbo/s_colTex/s_depTex/s_palTex`,
`s_cprog/s_cvao/s_cvbo`, `s_fbo2/s_colTex2/s_depTex2/s_dprog`), **all 31 uniform locations**,
`s_fboW/H/SS`, `s_palInit`, `s_uCast`, `s_shU`, and the three lane-gated upload blocks.

**Two of those three published CPU-side bytes outside the gate; the palette did not.** The fog
grid and the fog shade table each left their publication behind — which is why the blocks stayed
and only the texture calls went. The palette block was GL end to end and left nothing, because
the bytes a second backend needs are `tagpu_pal_live()`'s and each Vulkan twin uploads them
itself. Saying "each of those three" flattened a distinction the code makes explicitly.

**THE STRUCTURE-SHADOW GATE WAS PINNED SHUT, AND THAT IS THE FINDING.** It read
`tagpu_owndraw_set_structshadow(s_armed == 1 && gl_draws && suppress, …)`. The `gl_draws` term
existed so that a GL → Vulkan switch could not carry one raised frame across the seam. With one
lane there is no seam — and leaving the term in would have pinned the gate at 0 **for any future
painter**, so the engine would go on drawing its slant shadows underneath ours, silently. The
term is gone.

**And the gap that exposes, dated rather than guessed: nothing sets `s_ssSuppress` to 1 any
more.** Its writer was

```c
s_ssSuppress = tagpu_terrown_filled() ||
               (pdReady && ((mapLive && nterr > 0) ||
                            ((gfx & 4) && !(cpp && !hard) && npdSlant > 0)));
```

which stood in the GL draw half. Verified by reading two revisions rather than by memory:
present at `31c700d` (landing 10b), absent at `3771ec4` (landing 11-3). So the gate has
published 0 every frame since 11-3, the engine draws every structure shadow itself, and that is
the correct picture while we paint none. **A Vulkan-side structure-shadow painter must set
`s_ssSuppress` where the GL one did.** The read-and-clear keeps it bounded either way: a frame
that does not reach a painter lowers the gate on its own.

That same deleted region also answers, in its own words, a question the plan has been holding
open: *"`tagpu_shadow_begin` and `tagpu_shadow_end` are called only from this function below
that return … and so `tagpu_shadow_handover` returns 0 at `tagpu_vk_shadow.c:788` and that pass
stands down. No `tagpu_vk*` file mentions `slant` at all."* That is independent confirmation of
what a fresh grep also shows — `tagpu_shadow_begin` has no caller anywhere — and it sharpens
escalation reason 1 rather than resolving it.

**Fields we write: nothing moved, and three GL names went.** `s_palTex` had no reader once the
palette upload went, and **`s_fogTex`/`s_fogLutTex` turned out to have none either** — along
with the `fv.fogTex`/`fv.fogLut` write that was supposed to be their reason to stay.

That second one is worth the space, because it is the same mistake twice in one landing. The
two fog names were kept on the grounds that `tagpu_hires_draw.c` reads `v->fogTex` and
`v->fogLutTex`. It does — but those are **`TAGPU_HVIEW`** fields
(`inc/tagpu_hires_draw.h`), a different struct. `TAGPU_FXVIEW.fogTex`/`.fogLut` had no reader
anywhere in the tree. The first instance was `s_palTex`, where a comment claimed publication and
the compiler disproved it; the second was these two, where the claim survived the compiler
because the write existed and only the READ was missing, and it took the landing's review to
catch. **The rule both produce: trace the field, not the word.** The `TAGPU_FXVIEW` fields are
left in place with a tombstone — changing that struct's shape belongs to 11-5e — but nothing
should be plumbed into them believing there is a consumer.

The name collision that caused it is worth knowing on its own: `TAGPU_FXVIEW.fogLut` was an
`unsigned int` GL name, while the frame packet's `fogLut` is a `const unsigned char*` to the 256
bytes the Vulkan twins actually read through `tagpu_native_foglut()`. Same word, different
field, different type, different lane.

**Two smaller shapes.** `if (!gl_draws) { …hand over…; return; }` wrapped the entire remainder of
the function once 11-3 removed the draw half below it — so the test and the `return` went and the
hand-over is simply the end of the function. `tagpu_native_wrecks_armed` loses `s_state != 2`
("the GL program has not refused"), there being no program to refuse.

**Verification, same method as 11-5a.** Inventory against HEAD: exactly `getgl`, `init_gl`,
`mksh`, `tex2d` removed, nothing added — `main` defines 28 functions, this tree 24. Against
`main`, per function: **21 of the 24 SURVIVING functions are instruction-identical**, and the
three that differ are the three edited. (Four functions were deleted and cannot be compared;
"21 of 28" would read as seven changed, which is why the surviving count is the one to quote.)
External references lose **32 and gain nothing**, and the breakdown sums to 32 rather than past
it: **27** `gl*` entry points, `getgl`'s `GetModuleHandleA` and `GetProcAddress`,
`xwglGetProcAddress`, `tagpu_vk_owns_present` (no `gl_draws` left to compute) and
`tagpu_shadow_locate`, which `init_gl` called to name uniforms in the deleted program and which
still has two callers elsewhere. **Quote a total and a breakdown that adds up to it** — this note
first said "29 … plus three", which is 32 written as 34, the same double-count the 11-5a review
caught in §2.69. Include removal proved inert separately. Five of the six shaders
sit behind a probed pragma; `FS` is the exception with a live C caller,
`tagpu_native_unit_fs()`, which is why the posed and unposed units cannot drift apart.

### 2.71 The terrain pass loses its GL, and a whole route comes unpinned — landing 11-5c

`tagpu_terr.c` is GL-free: **107 call sites at HEAD, 0 now** (masked count, comments and string
literals blanked first), 541 lines out and 159 in. Gone: `init_gl`, `mksh`, `getgl`,
`glsl_begin`, `dump_if_armed`, `restore_step`'s GL half and `rgb_mirror_step`; seven `PFN_*`
typedefs and seven `x_gl*` pointers; **ten GL object names** (`s_prog`, `s_vao`, `s_vbo`,
`s_qvbo`, `s_atlasTex`, `s_rgbTex`, `s_hTex`, `s_hVao`, `s_hVbo`, `s_hIbo`); all eighteen
uniform locations; the bring-up's `s_state`; `s_shU`; and `opengl_utils.h`.

**The symbol table is the check, not grep.** Against HEAD's object compiled at the same path,
31 defined symbols out and 1 in, and the breakdown sums to 31: **10** GL object names, **7** GL
entry-point pointers, **7** read-back mirror fields, **2** the GL restore's job and state
(`s_job`, `s_state`), **1** the shadow read-back uniforms (`s_shU`), **1** the draw's corner
array (`corners.0`), **3** functions that survived `-O2` as symbols (`getgl`, `glsl_begin`,
`mksh`). The one added is `restore_publish`. Two further lines in the diff (`last`, `lastl`) are
the same function-scope statics renumbered because an earlier static was deleted, not removals —
which is exactly why a raw `diff | wc -l` of 33 is not the number to quote.

#### The finding: `tagpu_restorevk` was dead end to end, and had been since 4b-2

This is the largest instance yet of the shape that has now turned up in four consecutive
landings. `glsl_begin` did **two** jobs:

1. **API-independent** — compute the centre-out restore order (`restore_order`), build the
   per-tile `TAGPU_RGLSL_FRAME` list including each tile's tileability flag, and under
   `s_rvkWant` publish it as `s_rFrames` for a consumer to paint from;
2. **GL** — create `s_rgbTex`, spec it, and start a `tagpu_restoreglsl.c` job painting into it.

Its only two call sites were inside `restore_step`, whose **first statement** is
`if (!tagpu_classicpp_assets() || !s_atlasTex || !s_setPix) return;`. And `s_atlasTex`'s sole
non-zero assignment is `glGenTextures(1, &s_atlasTex)` inside `ensure_atlas`'s
`if (!tagpu_vk_owns_present())`. Traced at HEAD, not remembered: `glsl_begin` occurs **four**
times in the file — one definition, two calls at `:1221` and `:1249`, **and a mention in prose
at `:182`**; `s_atlasTex` is assigned at `:1014` under the gate at `:1012`; `restore_step`'s
return is `:1217`;
`tagpu_vk_owns_present()` is `return s_ownWin != 0`, set once before the frame loop
(`render_vk.c:145`) and never cleared.

So on the vulkan-only lane:

* **`tagpu_restorevk.on` published nothing.** The lever latched `s_rvkWant`, `tagpu_vk_terr.c`
  stood up its restore job, its atlas image and its painter — and the only writer of `s_rFrames`
  sat below a guard on a GL texture name. The consumer was armed and never fed.
* **`restored` was pinned at 0.** It read `(s_rgbState == 1 || s_rgbState == 2)`, and
  `s_rgbState` is written non-zero only inside `restore_step` below that same return. `restored`
  is what sets `uRestored` in the consumer's fragment shader (`tagpu_vk_terr.c:1676`), so even a
  consumer that *had* painted its restored atlas would have been told to sample the indexed one.

Both are now fixed by the same change. The order and the list are their own function,
`restore_publish` — no GL, no job — and `restore_step` drives it from `s_rvkWant` and the
palette serial. `restored` reads the published field itself:

```c
restored = (s_rFrames && tagpu_classicpp_assets()) ? 1 : 0;
```

`s_rgbState` keeps its name and loses a state: **0 none, 1 request published, -1 failed**. There
is no "complete" on this side, because completion is the consumer's fact and it already tracks
it (`s_rgbAtlas.have`, set when its own job reports painted frames).

**Default behaviour is unchanged, and provably rather than probably.** Without
`tagpu_restorevk.on` the lever never latches, `restore_step` returns on its first line,
`s_rFrames` stays NULL and `restored` is 0 — which is precisely what the pinned predicate
published before. The only path that changes is the one that was dead.

#### Two more predicates of the same shape

**`tagpu_terr_hills_draw`'s `!s_hVao` term.** The guard read
`if (!s_hVao || s_hMeshW < 2 || s_hMeshH < 2) return 0;`, and `s_hVao` is a GL vertex-array
name created only inside `build_hills`' lane gate. It sat **above** the publication of the CPU
caster mesh that the Vulkan shadow pass exists to receive — the function's own comment says "the
Vulkan shadow pass draws this and nothing else" — so on a lane with no GL it returned 0 before
reaching it. The two remaining terms are the mesh's own and are the ones that matter: after a
map change whose rebuild failed, the old mesh is still what the arrays hold and the new grid's
size would index past it.

**But the route is NOT open, and saying "instance 3" without this is half the story.** Removing
this term changes nothing observable today, because the pin above it is still in place:
`tagpu_terr_hills_draw`'s only call site is `tagpu_shadow_hills`, which is itself **called from
nowhere in the tree**, and which would in any case return at `!s_live` — and `s_live` is set
only past `tagpu_shadow.c`'s *own* GL bring-up, which latches failed on a lane with no context.
So this is a pin removed on a route with another pin in front of it. It is still worth removing
— a future producer would otherwise hit it after clearing the first — but the landing did not
open the route and neither the commit message nor the first version of this section said so.
Whether `tagpu_shadow.c`'s GL draw half is scaffolding or debris is exactly the question the
plan holds at **escalation reason 1**. [FROM THE 11-5c REVIEW.]

**The device bound, in `tagpu_terr_gather`** (not in `ensure_atlas`, which only reads
`s_maxTex` — a reader grepping the wrong function will not find it). The lane gate's GL arm
brought up the program and read `GL_MAX_TEXTURE_SIZE` into `s_maxTex`; its `else if` asked the
same of `tagpu_vk_max_image_dim()`. With the GL arm gone the bound is asked unconditionally, of
the device that will sample the atlas.

**A check went with the GL arm and had to be put back.** `init_gl` also refused a device whose
limit was under `ATLAS_W` (2176), once, with a log line. Without it `ensure_atlas` clamps `rows`
to `m / CELL_PITCH` and builds an atlas the consumer's own image creation then refuses every
frame with nothing saying why. Restored in the gather, against the device's own 2D limit. No
conformant device takes that arm — Vulkan floors `maxImageDimension2D` at 4096 — which is why
it is a **bound** rather than a prediction about which devices exist. [FROM THE 11-5c REVIEW.]

#### The read-back mirror goes with the texture it read

`s_rgbMirror` and its six companions were the *other* way of feeding the same consumer: pull the
GL twin's restored atlas back to the CPU through `tagpu_gaf.c`'s helper so a second backend could
upload it. That only ever made sense while a GL lane painted it. `tagpu_terr.h`'s
`atlasRgb`/`atlasRgbRows` are now always NULL/0, so the header's claim that the two hand-over
fields are **mutually exclusive** holds by construction rather than by the latch order that
landing 7c's review had to repair.

#### Fields we write

Nothing moved. Ten GL names left the file and none of them was a hand-over field. The hand-over
loses two fields to permanent zero (`atlasRgb`, `atlasRgbRows`, with `atlasRgbSerial` following
them) and gains no new one; `restoreFrames`/`restoreN`/`restoreSerial`/`restoreRepaint` are
unchanged in shape and reachable for the first time.

#### Measured by running it, 2026-09-19 — the route works end to end

Not an object-identity check: 11-5a and 11-5b were inert and could be settled by comparing
emitted code, but this landing changes behaviour on the lever path, so it was run. Two Continents
at 1024×768 on a private Xvfb display (`TACLI_DISPLAY`), `renderer=vulkan`, armed
`terr.on=log classicpp.on restorevk.on`. The log, in order:

```
terr: restorevk -- the restored atlas is the other lane's to paint, …
terr: atlas built 2176x2720 for 5062 tiles
terr: restore request published -- 5062 frames, 2176x2720 atlas, serial 2
restorevk: the device can restore: 12x64 fp32, one k-block 9 KB of a 64 KB block …
restorevk: terr: job started: 5062 frames (400 wrap-padded), model 12x64
vk: terr: restoring the tile atlas HERE - 5062 frames over 2176x2720, serial 2
vk: terr: restored atlas painted here - 5062 frames, no mirror and no read-back
vk: census: frame 900:  1 pass(es) drew … terr=1
vk: census: frame 1200: 1 pass(es) drew … terr=1
vk: census: frame 1500: 1 pass(es) drew … terr=1
```

**That third line has never been printed by any build before this one.** It is the producer that
landing 4b-2 shut behind `!s_atlasTex`, and the four lines under it are the consumer that has
been armed and unfed ever since. The whole atlas — all 5062 tiles — is restored on the Vulkan
lane with no read-back and no mirror, which is what `tagpu_restorevk.on` was built to do.

And `terr=1` on every census after the map loads is the other half: **the terrain draws.** That
is the check the review's HIGH would have failed — before the fix, a lane that declined the
restorer drew no terrain at all, and a lane that had not yet painted drew none either.

The pass's own A/B capture (`tagpu_terr.ab`, `tagpu_terr_vk.ppm`, 2048×1536 at `ss=2`) shows the
expected picture: restored colour inside the unit's line of sight, and the engine's grey fog rule
outside it, over the whole heightfield. 82 % of the frame is non-black; the black is the letterbox
and the unexplored border.

**A note on what `tacli shot` shows here, because it reads as a failure and is not.** The engine
surface comes back CYAN across the viewport: `terrown: engine terrain + fog overlay SKIPPED (ours
live)` means the engine's terrain is replaced by the key fill, which the Vulkan lane then
composites over. `tacli shot` captures the engine surface, and `glshot` is retired with the GL
lane, so the pass's own `.ab` capture is the way to see what this lane actually drew.

#### What the review found: making the producer honest fired the consumer's parity rule

Two dedicated reviewers, twenty-three findings, all verified against the source before acting.
The two HIGHs are one defect, and **this landing introduced it**.

`tagpu_vk_terr.c` stood the whole terrain pass down whenever a restore request stood that this
lane had not painted:

```c
if (t.restored && !(s_rgbAtlas.view && s_rgbAtlas.have && (… || t.restoreFrames))) return 0;
```

That was right while a GL twin drew the restored atlas beside us — drawing indexed then would
have put a different picture on the screen from the one the twin drew, and holding the two lanes
to the same picture is what the A/B is for. **There is no twin.** And every way the restorer can
decline — `tagpu_vk_restore_up` refusing a format or failing to load the model file, `job_new`
returning NULL, `job_add` refusing, a job that failed before its first slice — latches
`s_rjTried`, which is **one-way for the device's life** (cleared only in `tagpu_vk_terr_down`).
After that `s_rgbAtlas.have` can never become 1. So the stand-down was **permanent: no terrain
at all for the session**, announced once and silent after that. `tagpu_vk_restore.h` promises
that such a refusal leaves "Classic++ indexed on this lane — the shipped fallback, not a fault";
this had turned it into a black world.

Before this landing `restored` was pinned at 0, so the branch was unreachable. **The rule this
gate named cuts both ways: unpinning a predicate makes every consumer of it run for the first
time, and a consumer written for two lanes may have a rule that only made sense with a twin.**

Fixed where the fact lives. `uRestored` is now the consumer's own
`t.restored && s_rgbAtlas.view && s_rgbAtlas.have`, so a request this lane cannot service, or
has not serviced yet, draws **indexed**. The flag and binding 42's descriptor now derive from
the same two facts and cannot disagree — which is a stronger guarantee than the stand-down gave,
and local to the line that needs it. During a restore it is also exactly the centre-out reveal:
the fragment shader's alpha test picks restored or indexed **per cell**.

Three more, each verified:

* **The stale atlas.** Past a serial change, every path that gives up now drops `have`. What the
  image holds was painted for the *previous* request — the previous palette, or the previous
  map's rectangles — and leaving it set is a silently wrong picture rather than a missing one.
  Dropped at the give-up sites and not up front, because `repaint` reads it: clearing it early
  would turn every palette change into a full blank-and-repaint, which is the one thing the
  repaint path exists to avoid.
* **The repaint throttle, found by both reviewers independently.** HEAD gated the repaint on
  `s_rgbState == 2` — "the job is idle" — a completion this side can no longer see, because the
  painting is the consumer's. Collapsing the state machine made it fire on every palette change,
  and `tagpu_pal_serial()` bumps once a frame during a fade: the whole frame list republished
  per frame, `restore_order`'s full tile-map scan (up to 4.19 M cells) and a
  `tagpu_rglsl_tileable` over every tile each time, and the consumer restarting its job with its
  paint count back at zero — so the restore made **no progress for the length of the fade**. Now
  gated on the palette having SETTLED: two consecutive frames reading the same serial, which is
  an ordering on the palette's own serial and **not a timer**. Residual, stated in the code
  rather than hidden: a palette that settles, moves and settles again *during* a restore still
  restarts it, and closing that needs a completion signal back from the consumer.
* **Two bounds.** `restore_publish` refuses `n < 1` — `malloc(0)` may return non-NULL, so a
  zero-tile atlas would have published "a restore stands" with a list of zero frames while the
  consumer's own test is `restoreFrames && restoreN >= 1`, the two sides disagreeing across the
  seam about one fact. And the atlas-width refusal above.

#### And what the RE-review found: the fixes had two defects of their own

A second round on the fix commit alone. No HIGH, the main fix confirmed sound — and two
real defects introduced by the fixes themselves, which is why a fix round gets reviewed
rather than assumed.

**The bound did not bind.** The atlas-width refusal set `s_maxTex = m` and then bailed — but it
lives inside `if (s_maxTex <= 0 || s_maxTex != tagpu_vk_max_image_dim())`, so from the very next
frame that condition was false, the block was skipped entirely, and `ensure_atlas` went on to
build the atlas the check exists to prevent. The log line reading *"terrain stays the engine's"*
was true for exactly one frame. `terr_bail()` latches nothing; the pre-11-5c original latched
`s_state = 2`, and I replaced a latching refusal with a non-latching one without noticing the
difference. Now the refusal leaves `s_maxTex` alone, so its own condition stays true and it is
re-applied every frame, and **the log is what latches** — on the device's reported value
(`s_dimBad`), so a re-picked device is asked again on its own merits. **A bound that does not
bind is still a defect**, even where no conformant device reaches it.

**A layout nobody had set.** Removing the stand-down let the pass draw on a frame where binding
42 named an image that has never left `VK_IMAGE_LAYOUT_UNDEFINED`. `shared_resize` creates it
UNDEFINED and only the restorer's own job transitions it (`UNDEFINED → TRANSFER_DST →
SHADER_READ_ONLY`), so on the **declined-restorer path — the exact path the stand-down fix exists
for** — the image is created, no job is ever built, and it sits UNDEFINED for the session while
the pass now draws every frame. The shader guards the read, so nothing wrong is displayed, but it
is a descriptor-layout mismatch on a statically-used binding: the validation layers report it,
and the spec leaves it undefined if `uRestored == 1 ? texture(…) : vec4(0.0)` compiles to a
select rather than a branch.

Binding 42 now names the restored view only when `.have` — which cannot be set before the job's
clear has run, so **testing it is exactly "this image has left UNDEFINED"**. That also makes true
a sentence the fix round had already written and this round caught as overstated: the flag and
the descriptor now genuinely come from the same two facts, rather than from one fact and a
refusal placed elsewhere.

**The debounce claim was too strong.** `palWas == s_palSeen` is "the last two *calls* read the
same serial", and the render loop wakes on every primary Blt/Flip/Unlock as well as on a palette
change — so it usually runs more than one iteration per palette step, and a slow fade can still
get one republish per step. What it closes is the worst case, a palette moving on every
iteration. **The real fix is a completion signal back from the consumer**, which `tagpu_terr.h`
does not carry: the GL lane never needed one because it owned the job and could see it go idle.
The comment now says what it buys and names what would close it.

**Comment truth, second pass.** Six cross-references in `tagpu_vk_terr.c` still reasoned about
"the refusal below" that the fix deleted, two of them load-bearing. `tagpu_terr.h`'s `restored`
contract still claimed it sets `uRestored` — it does not, and a consumer that believed it would
sample an image nothing has written. The `glreset` unreachability note named the far end of the
caller chain (`tagpu_overlay.c`) as "its one caller", when the actual caller is
`tagpu_native_glreset`, which tests nothing. The failure log promised *"what it painted stands"*,
which the new `have`-drop makes true only until the request changes. And the retained-list cost
was decimal KB in a file that reports KiB, with a largest-stock-map figure that needs a device
limit most cards do not have (217 KiB on Two Continents; 331 KiB at the ceiling a 4096-limit
device allows).

**Re-verified by running it after the fixes**, same fixture: `restore request published — 5062
frames`, `job started`, `restoring the tile atlas HERE`, `restored atlas painted here — 5062
frames, no mirror and no read-back`, `terr=1`, and an `.ab` capture identical to the pre-fix one
(24.6 % saturated colour — restored inside the line of sight, grey fog outside). Gating binding
42 on `.have` did not break sampling.

#### There is no thread split in this pass, and the brief that said so was wrong

Worth recording because it is load-bearing and it was asserted confidently in the wrong
direction. **Producer and consumer are the same thread**, in one iteration of the render loop:
`render_vk.c:232 tagpu_overlay_draw()` → `tagpu_overlay.c:401 tagpu_native_frame()` →
`tagpu_terr_gather()` / `tagpu_terr_render()` — where `restore_step`, `restore_publish`,
`rlist_drop` and `terr_publish` all run — **then** `render_vk.c:268 tagpu_vk_frame()` →
`tagpu_vk_terr_prepare()` → `tagpu_terr_handover()` → `tagpu_vk_restore_job_add`.

So `s_rFrames` is not a cross-thread hand-over, and neither are `atlas` and `height`: the bound
is the sequential ordering inside one iteration, not the frame stamp in `tagpu_terr_handover`
(which still earns its keep for the *bail* case — a gather that returns early leaves `s_pubHave`
set while `ensure_atlas` frees the mirror). `tagpu_vk_restore_job_add` **copies** the frames, so
the only dereference is inside that one call. **`tagpu_reclaim` is not applicable**: it defers
*engine* frees made by the *game* thread; this is our own heap on the render thread.

#### What the landing cost: terrain's half of the byte oracle

`dump_if_armed` was the only producer of terrain's `tagpu_restoredump.on` dump, the two-lane
byte comparison `ta-drive` calls "strictly stronger than the cross-build one"
(`.claude/skills/ta-drive/SKILL.md`, `tascene-design.md`, `renderers.md` §4c Q8). It read a GL
texture back with `glGetTexImage`, so it could not survive the lane. `tagpu_gaf.c` still writes
the feat/fx/unit halves, so **those pairs survive and terrain's alone is gone** — which is the
least obvious way for an oracle to break. Nothing replaces it yet; the Vulkan-side equivalent
would have to read the consumer's image back, and that belongs with whoever next needs the
comparison rather than with this landing.

#### Two things this section first got wrong, and what they cost

**"Three occurrences" was a count of `glsl_begin` read as "one definition, two calls".** The
fourth was prose — and the miscount and the stale comment it hid are the *same* defect: that
prose was at the declaration of `s_rFrames`, saying "the list is the one `glsl_begin` built for
the GL job", which is the very list this landing made reachable. A grep whose result is read as
a conclusion rather than as a list is how a rewritten-comment pass misses the one comment that
matters. [FROM THE 11-5c REVIEW.]

**"Every GL site sat under `if (!tagpu_vk_owns_present())`" — false for 17 of the 107,** and the
landing's own headline finding is the counterexample. Only `ensure_atlas` (10), `build_height`
(10) and `build_hills` (12) sat under it literally, and `init_gl` + `mksh` (58) were reached
through the gather's arm — 90. The other **17** were unreachable through a *second* step, a
guard on a GL object name: `glsl_begin` (9) and `dump_if_armed` (4) behind `restore_step`'s
`!s_atlasTex`, `tagpu_terr_hills_draw` (3) behind `!s_hVao`, `rgb_mirror_step` (1) behind
`!s_rgbTex`. That two-step shape is the rule this gate named, so flattening it into "one gate"
erases the thing worth reading. The commit message carries the flat version; this is the
correction, and the commit stands as written because history is not rewritten here.

#### Verification

Build clean; `thread-split: clean — 34 listed file(s)`; `spirv: 49 shaders, 33 programs, headers
current`. Function inventory against HEAD: exactly `dump_if_armed`, `getgl`, `glsl_begin`,
`init_gl`, `mksh`, `rgb_mirror_step` removed and exactly `restore_publish` added. `VS`/`FS` are
the **Vulkan** terrain shaders' source of truth — `tools/spirv-gen.py` manifest entries
`tagpu_terr::VS` and `tagpu_terr::FS`, read out of the preprocessed translation unit — and sit
behind a pragma whose `pop` was **proved** by planting an unused static after it and compiling
(a `-fsyntax-only` run does not surface `-Wunused-variable`; a real compile is required).

### 2.72 The posed program loses its GL, and its readiness answer stops flattering the engine — landing 11-5d

`tagpu_posedraw.c` is GL-free: **122 call sites at HEAD, 0 now** (masked count, comments and
string literals blanked first), 483 lines out and 122 in. Gone: `getgl`, `mksh`, `link_block`,
`upload_pose`, nine `PFN_*` typedefs and nine `x_gl*` pointers, three GL object names
(`s_prog`, `s_dprog`, `s_ubo`), **all forty-four uniform locations** (33 body, 11 depth),
`s_shU`, and `opengl_utils.h`.

**Forty-four is the SOURCE count and twenty-five is the OBJECT count; this paragraph said
twenty-five for one commit and was mixing the two.** Only 14 of the 33 body locations survived
to the symbol table, because landing 11-3 deleted the per-unit uniform sets and gcc had already
dropped the other 19 as write-only. The *Verification* section below uses the object figure and
is right to — but a source inventory has to count the source.

**The commit message says 123 and the number is 122.** 123 is the raw grep; the masked count —
comments and string literals blanked before counting, which is the count this section quotes
throughout — is 122, and the one difference is a `glBindVertexArray(0)` named inside a comment
in `tagpu_posedraw_end`. History is not rewritten here, so the commit stands and this is the
correction.

**Ten of the fourteen functions had no caller anywhere in the tree before this landing began** —
`tagpu_posedraw_{shadow_begin,shadow_set,redraw,slant_begin,slant_set,slant_redraw,wire_begin,wire_unit,depth_begin,depth_unit}`,
62 GL calls between them. Their caller was `tagpu_native.c`'s GL composite, deleted by landing
11-3; landing 11-3 removed the calls and left the callees. Their declarations went from
`inc/tagpu_posedraw.h` with them.

**The one deletion that needed an argument was `upload_pose`,** because unlike the rest it sat on
a live path (`tagpu_posedraw_unit`) with its GL behind a *second* guard — the two-step shape
§2.71 named. It is inert to remove because `pose_words` is **pure**: it fills two
caller-provided arrays and returns a clamped count and holds nothing between calls. `upload_pose`
called it, and `pd_record` calls it again — the same answer computed twice, not two halves of one
sequence. What `upload_pose` then did with the answer was write it into a uniform buffer nothing
binds.

**What the Vulkan lane does and does not replace.** The wire, the Classic silhouette and the
structure slant are not ported — that pass draws the body range only and says so under its own
*WHAT IT DOES NOT DO* (`tagpu_vk_unit.c:90`). **All ten entry points are a tombstone, not a
move**, and the gap belongs to `tagpu_vk_unit.c`.

**THIS SECTION SAID "The depth twin IS ported" FOR ONE COMMIT AND THAT WAS WRONG** — caught by
this landing's review, which was right that it is the sentence most likely to stop the next
session looking. The Vulkan code exists: `tagpu_vk_unit.c:1043-1046` builds the `pose_depth`
pipeline from the `VS` + `DFS` that stay in `tagpu_posedraw.c`, and `:1769` sets `uDepthPass` 1.
**Nothing reaches it.** `tagpu_posedraw_depth_begin` held the only `s_depthOn = 1` and the only
write of `s_depthMat` in the tree, so with it gone:

| link | result |
|---|---|
| `s_depthOn` | only ever assigned 0 (`tagpu_posedraw.c:1108`) |
| `TAGPU_PDUREC.casts` | always 0 (`:814`) |
| `TAGPU_PDHAND.depthOn` / `.castMat` | always 0 / the `memcpy` at `:707` that never runs (`:706`) |
| `tagpu_vk_unit.c:2358` | `w->casts` always 0, so `s_ncast` never increments |
| `tagpu_vk_unit_cast` | returns at `!s_ncast` (`:2579`) every frame |
| `build_cast_pipeline` | its **only** caller is `:2587`, inside that function |

So the pipeline is never built and **no posed unit casts into the Vulkan cast-shadow map**.

**This is not a regression from 11-5d.** `_depth_begin` was already uncalled at landing 11-3, so
`depthOn` has been pinned 0 since then and no behaviour changed here. What 11-5d did was delete
the last code that could ever set it — turning a dormant path into a dead one — and then briefly
call it ported.

**And the census cannot see it** — though not for the reason this paragraph first gave.
`tagpu_vk_shadow.c:847` asks `h.otherCasters - ours > 0`. `otherCasters` has **three** writers,
not two: `tagpu_shadow_unit` (`:432`) and `tagpu_shadow_note_casters` (`:438`), both callerless,
**and the heightfield-mirror-missing path at `:473`**, which both G19e shadow reviewers added
precisely so the census covers all four kinds of caster. That third one is live and reachable,
so "0 − 0" is not true in general: when `build_hills` takes its out-of-memory exit the census
fires and refuses the whole map with a log line, which is the design working.

What is true is narrower and is still the point: **a missing UNIT caster can never reach
`otherCasters`**, because the GL lane that counted them is gone and its two incrementers went
with it. So on an ordinary frame the term is `0 − 0`, the map is published as complete, units
cast no shadow and nothing logs it. [The third writer was found by this landing's review; the
first version of this paragraph overclaimed.]

#### The finding: the engine is not a fallback

`tagpu_posedraw_live()` was `s_state == 1 && !tagpu_vk_owns_present()`. The second term is pinned
false (`tagpu_vk.c:732`), so it has always returned 0. **The 4b-2 landing review put it that way
on purpose and its argument stands**: this pass hands over rather than draws, so `s_state == 1`
alone would promise a draw the consumer can decline — the twin stands down on a short hand-over,
a missing mirror or `TAGPU_PD_MAXHAND`, and every such frame would be a unit the game thread had
already told the engine not to draw.

What did **not** stand is the comfort attached to it — *"the engine keeps its own rasterise"*,
read by every later reader, and written into `tagpu_owndraw.c`'s own paragraph, as "so the unit
is still on the screen in 8bpp".

**MEASURED 2026-09-19** — `one-unit` on Two Continents, 1024×768 on a private display, one
ARMCOM at screen (512,384), instance DLL md5-matched to the build, **on both lanes**:

| configuration | TA's own surface (`tacli shot`) | the PRESENTED frame (window capture) |
|---|---|---|
| `renderer=vulkan`, everything armed | commander, in colour, with its drop shadow | ours (the posed body), not the engine's |
| `renderer=vulkan`, `native.on` off | the same | **absent** — our marker cross and health bar over empty ground |
| `renderer=vulkan`, `native.on` **and** `terr.on` off | the same | **still absent**, while TA's own terrain *does* reach the frame (it comes up green) |
| **`renderer=gdi`**, `native.on` + `terr.on` off | the same | **PRESENT** — the window capture and the engine surface are the same picture |

The engine rasterises every unit in all four: `OWND target=all skipped=0 passed=55991` and
climbing on the Vulkan lane, and on gdi the detours are armed too (`owndraw: ARMED target="all"`)
while `tagpu_posedraw_live()` is a literal `return 0`, so `classify` cannot skip on either.
(Rows 1 and 2–4 are different configurations, not one run: with `native.on` off, `g_passed`
increments at `tagpu_owndraw.c:398` on `!target_covers` rather than at the `!live()` branch. The
engine rasterises either way, which is the part the table is for.)

#### THE LAST ROW IS THE FINDING, AND IT REVERSES THE FIRST WRITE-UP

This section concluded, for one commit, that *"the engine's per-unit rasterise is invisible
work"* — that the engine's output was inherently unable to reach the player. **The gdi control
disproves that**, and it was asked for by this landing's review after the first three rows had
been written up alone. The same engine output reaches the player perfectly well one lane over.

**So the loss is OURS and it is in the Vulkan composite path.** Not a property of the engine, not
a property of the design, and fixable. Restated:

* the 4b-2 comfort — "the engine keeps its own rasterise", so a unit we fail to draw is still on
  screen — is **TRUE on gdi and FALSE on Vulkan**, and false because of a defect of ours;
* `tagpu_posedraw_live()` returning 0 is therefore not merely safe, it is **load-bearing, and
  more so than 4b-2 knew**: while the composite drops TA's units, a `live()` of 1 would take away
  the engine's copy too and a stood-down frame would have nothing on it at all;
* once the composite is fixed, the fallback is real, as gdi already shows.

#### The mechanism: a lead, not a finding

Not established, and this landing does not guess. The lead comes from reading rather than
measuring, and it fits every row above:

**[The lead below was MEASURED AND CLOSED 2026-09-20 — §2.82. The race was real (223 of 16 500
reads torn at that site) and the capture now runs on the game thread. Whether it was the
mechanism of THIS section's symptom was never established, so this stays a lead.]**

the capture, then `tagpu_surf_take` (`tagpu_surf.c:32`, called from `tagpu_overlay_draw`;
the name and the line are both gone since 2026-09-20 — it is `tagpu_surf_capture` on the game
thread) copies
`g_ddraw.primary->surface` **on the render thread**. `tacli shot` reads the same object via
`ss_shot_service(g_ddraw.primary)` (`tagpu_overlay.c:499`) at the **entry of the engine's flip,
on the game thread**, where its own comment says *"the primary here holds the frame the PREVIOUS
flip presented"* — a finished frame. `tagpu_surf.h`'s argument is explicitly a **lifetime** one
(`g_ddraw.cs` keeps the pointer live; `dds_Flip` swaps inside it so the row loop never splices
two buffers) and says nothing about the buffer holding a finished frame — and TA writes those
bytes from the game thread without entering that section. A snapshot landing mid-draw gets what
TA has drawn so far. The UI surviving is consistent rather than contradictory: on the presented
frame it does not come from this snapshot at all but from the GUI pass's mirror hand-over, which
`tagpu_surf.h` says was the only route TA's surface had before 4c.

**How to settle it:** dump the snapshot's own bytes in the frame `tacli shot` fires and diff
them. **If it holds, the fix is an ORDERING** — snapshot where the shot does, at the flip on the
game thread, where the frame is finished by construction — never a timing mitigation
(`CLAUDE.md`, *Fixes must be safe by construction*). This is not 11-5d's to fix: it is a
cross-thread change and belongs in a landing whose review is briefed on it.

**So the predicate keeps its value and loses its disguise.** It is `return 0` with the
measurement written at it, rather than a lane test that reads as something which comes back when
the lanes do. Value-identical by construction — the term removed was pinned false — so no
consumer runs for the first time, which is the trap §2.71's corollary names. `s_state` is
deliberately not consulted: the pass arming has never been the question this answers.

#### Verification

Build clean; `thread-split: clean — 34 listed file(s)`; `spirv: 49 shaders, 33 programs, headers
current`. Function inventory against HEAD removes exactly those fourteen and adds nothing.
`i686-w64-mingw32-nm --defined-only`, on the same TU compiled at an identical path, goes **119
defined symbols to 65**, and the 54 sum: 11 depth-program locations, 14 body-program locations, 8
loaded entry-point pointers — **eight, not the nine in the source**, because `x_glGetIntegerv`
was already unused at HEAD and gcc had dropped it there too — `s_prog`/`s_dprog`/`s_ubo`/`s_shU`, the ten entry points, `getgl`,
`mksh`, `upload_pose` and its two function-scope statics. **Two of the 54 are not deletions** —
`pose_words` and `unit_ok` are gcc inlining them at their now-single call site, and both are
still in the source; `link_block` was already inlined at HEAD and so never appears. `VS` and
`DFS` are the **Vulkan** posed shaders' source of truth (`tools/spirv-gen.py:186-187`,
`pose_unit` = `tagpu_posedraw::VS` + `tagpu_native::FS`, `pose_depth` = `::VS` + `::DFS`) and sit
behind a pragma whose `pop` was **proved** by planting an unused static after the include and
compiling.

#### Not covered by 11-5d

* The mechanism above.
* The **cloaked** case. `frag = vec4(rgb * uAlpha, uAlpha)` with `TAGPU_PDUNIT.alpha` 0.5 while
  cloaked means our body writes alpha 0.5, so whatever is under it survives at 50 %. What that
  looks like was not measured; the fixture used no cloak.
* **Colour was not read off the presented frame.** On the private display the world our passes
  draw comes up monochrome while TA's own frame is in colour — both halves of that are visible in
  the second control. A private display is software GL and the project's standing rule is that
  it is for numbers, not pictures ([tacli design](../tacli-design.html): *"Not Xvfb/Xephyr (software
  GL, unwatchable)"*), so no colour conclusion is drawn from it and the monochrome is recorded as
  an observation rather than a finding. Every claim above is a presence/absence one, which that
  display does answer — and answers twice, since TA's own frame comes through it in colour.
* **The posed cast-shadow path itself.** The chain above says nothing reaches the `pose_depth`
  pipeline; giving `depthOn` a producer is new work on `tagpu_vk_unit.c`, not a deletion, and it
  is not in this landing. Nor is the `otherCasters` census, which cannot fire for the same
  reason on its own side.
* **Three symbols this landing orphaned**, none deleted, all still compiled into the DLL. A
  non-static that loses its last caller compiles clean under `-Wall`, which is the inverse of
  the trap in *Verify deletions by inventory*, so they are named here rather than counted:
  `tagpu_native_unit_fs()` (`tagpu_native.c:555`, declared `inc/tagpu_native.h:12`) — its only
  call was the deleted GL bring-up, and `tools/spirv-gen.py` reads `tagpu_native::FS` out of the
  preprocessed source rather than through this accessor, so nothing needs it; and
  `tagpu_shadow_locate()` with the `TAGPU_SHADOWU` struct (`tagpu_shadow.c:593`,
  `tagpu_shadow.h:50-51`) — the deleted bring-up was its only caller in the tree. Its sibling
  `tagpu_shadow_apply()` was already orphaned. `tagpu_shadow.c` is escalation reason 1, so
  nothing there is touched here.
* **`tagpu_posedraw_stats()` and its four counters.** No caller since landing 11-3, and 11-5d
  removed the last increments of the slant and wire pair. `ta-drive` was telling sessions to
  read all three fields out of the log; corrected in the same commit as this note.
* **The composite defect the gdi control exposed**, and the snapshot lead above. A cross-thread
  fix belongs in a landing whose review is briefed on it, not folded into this one.
* **A stale line citation in `tools/spirv-gen.py`** (`tagpu_posedraw.c:432-434` for VS/DFS,
  which moved with this landing). **Left deliberately.** `tool_hash()` is
  `sha256(pathlib.Path(__file__).read_bytes())` — the SHA-256 of the WHOLE FILE — and it is one
  of the three hashes every generated header carries, so editing even a comment there fails
  `spirv-check` on all thirteen headers with *"generated by a different spirv-gen.py"* and
  requires regenerating them through glslang. Measured by doing it: the comment fix broke the
  build gate and was reverted. Note the docstring says the hash exists *"so a change to the
  TRANSFORM invalidates every committed header"* — the implementation is stricter than that
  sentence, and a comment is not a transform.
* `tagpu_posedraw_glreset()` still exists and still clears `s_state`; retiring the `*_glreset`
  surface is 11-5e.

#### One review finding checked and NOT upheld

The review flagged, as PLAUSIBLE, that dropping the `if (tagpu_vk_owns_present())` wrapper from
`tagpu_posedraw_ready()` changes behaviour where the Vulkan lane is `ST_READY` but does not own
present — `ready()` would answer 1 there, while the mirror latch in `_frame` is still gated on
`tagpu_vk_owns_present()`, so every record would fall to `s_other++`. Cost, not corruption.

**That configuration is not reachable, and the argument is in the source rather than in this
landing's head.** `tagpu_vk.c:509-512`, written by the landing that removed route D, states it:
*"`renderer=vulkan` reaches `render_vk.c`, which calls `tagpu_vk_own_present()` before its loop,
and nothing else drives this lane: `render_ogl.c`'s own call to `tagpu_vk_frame` went with the
popup."* `tagpu_vk_own_present()` has exactly one caller (`render_vk.c:145`), it precedes the
frame loop, `s_ownWin` is never cleared, and `tagpu_vk_max_uniform_range()` returns 0 unless
`lane_state() == ST_READY` — which only happens inside that loop. So at every point where the
accessor can answer non-zero, line 145 has already run. Recorded rather than dismissed: the next
person to remove a lane test deserves the citation and not a second search for it.
* The `glReadPixels` the restored-atlas step still runs (`tagpu_gaf.c`, via
  `tagpu_r3d_atlas_mirror_rgb_step`) — the comment in `tagpu_posedraw.c` that cites it is still
  true today and 11-5e is what makes it stale.

### 2.73 Three leaf files go GL-free, and a watchman with no door takes a reset tree with it — landing 11-5e-1

`tagpu_ftime.c`, `tagpu_text.c` and `tagpu_overlay.c` are GL-free: **52 call sites at the
previous commit, 0 now** (masked count — comments and string literals blanked first), and the
tree's surface goes from **592 sites in nine files to 540 in six**. Thirteen functions are
gone. The deletions are the small half of this landing; the structural find below is the
large one.

**The two counts are taken on both trees by one script with one regex over one file set**,
not quoted from an earlier section. All three have to be stated or the number does not
reproduce:

    pattern:   \b(?:gl|x_gl)[A-Z][A-Za-z0-9]*\s*\(
    over:      comment- and string-masked source
    file set:  tagpu/ddraw/src/*.c          (NOT the headers)

run against a `git archive` of the previous commit and against the worktree. The file set is
load-bearing: adding `tagpu/ddraw/inc/*.h` brings in `glcorearb.h`, which alone reads 1251,
and the same pattern then returns 1791 rather than 540.

The wider pattern that was run by accident inside this landing —
`\b(?:gl|x_gl|oglu_|wgl|xwgl)[A-Za-z_][A-Za-z0-9_]*\s*\(`, allowing a lower-case letter after
the prefix — reads **689** over the same file set, because it counts `glyph_raster`,
`glreset` and friends. **A number compared against one measured differently is not a
comparison**, and an earlier draft of this paragraph described that pattern in words rather
than writing it out, which made the 689 unreproducible. [The 11-5e-1 review's LOW.]

#### What was deleted, and how each was shown to be unreachable

Every one under a masked-comment caller scan across `tagpu/ddraw/src/*.{c,h}` and
`tagpu/ddraw/inc/*.h`, each showing exactly its declaration and its definition and no call:

| function | file | what it was |
|---|---|---|
| `tagpu_ftime_gl_begin` / `_gl_end` | `tagpu_ftime.c` | the GL lane's timestamp bracket; its one caller was `render_ogl.c`, deleted in 11-2 |
| `tagpu_overlay_capture_begin` / `_capture_end` / `_target_fbo` | `tagpu_overlay.c` | the glshot capture into an FBO we owned; same caller |
| `tagpu_overlay_glreset` | `tagpu_overlay.c` | forgot the capture's ids on a context change |
| `tagpu_text_tex` / `tagpu_text_glyph_tex` | `tagpu_text.c` | the string and glyph atlases' GL textures |
| `tagpu_text_glreset` | `tagpu_text.c` | dropped both ids |
| `tagpu_mark_glreset` | `tagpu_mark.c` | its whole body was one forward to the line above |
| `oerr` | `tagpu_overlay.c` | a five-site `glGetError` probe behind `tagpu_gldbg.on` |
| `getgl` × 2 | `tagpu_ftime.c`, `tagpu_overlay.c` | the per-file GL loader |

With them, **34 statics** (the object files' count, so the two function-scope ones are in
it): 8 `PFN_*` typedefs and their 8 `x_gl*` pointers, the GL query pool (`s_qBeg`, `s_qEnd`,
`s_qPend`, `s_qSlot`, `s_qMade`, `s_glOpen`), the `GL_TIMESTAMP` / `GL_QUERY_RESULT` /
`GL_QUERY_RESULT_AVAILABLE` defines, `s_glFetched` / `s_glOk` / `s_glSaid` / `s_glDrives`, the
second sample ring (`s_glRing`, `s_glN`, `s_glAt`, `s_glTotal`), the capture's
`s_capFbo` / `s_capTex` / `s_capW` / `s_capH` / `s_capArmed` and the watch's `s_ctx` and
`pwgc`, `oerr`'s `pge`, `s_tex` / `s_gtex` / `s_dirty` / `s_gdirty`, three
`#include "opengl_utils.h"` lines and one `#include "tagpu_overlay.h"` (in
`tagpu_render3do.c`, whose comment said it was there for `tagpu_overlay_target_fbo` — a
function that file never called).

#### THE FIND: a watch on a door that is not in the building

`tagpu_overlay_draw` ran this once a frame, at the top, before anything else:

```
HGLRC cur = pwgc ? pwgc() : 0;          /* wglGetCurrentContext */
if (cur != s_ctx) {
    if (s_ctx) {
        s_state = 0;
        tagpu_overlay_glreset();  tagpu_native_glreset();
        tagpu_scaffold_glreset(); tagpu_r3d_glreset();
        tagpu_gui_glreset();      tagpu_fps_glreset();
    }
    s_ctx = cur;
}
```

It was written for a real failure — the fork restarts its render thread with a new GL context
on a display-mode change, and a stale FBO bind then cleared the real backbuffer black (found
during the 1024x768 resolution test). **It cannot fire in this build**, and the invariant is
a CALLER rather than a name: **`oglu_load_dll()` -- the only
code in the tree that resolves `wglCreateContext` and `wglMakeCurrent` -- HAS NO CALLER**,
and neither resulting pointer is ever invoked (`opengl_utils.c:133` is a truthiness test).
`dd.c`'s renderer selection has four arms resolving to two destinations — `'d'` and `'s'`/`'g'`
to `gdi_render_main`, `'v'` and everything else (including `'o'`, auto and unrecognised) to
`vk_render_main` — and **none of them probes GL or calls `oglu_load_dll`**. (An earlier draft
said "exactly two arms". The conclusion is what matters and it holds for all four, but a count
that is wrong invites the reader to check whether a fifth was missed. Round 3's LOW.) The
game's own import table names no `opengl32`.

The first draft of this paragraph said instead that `wglCreateContext` and `wglMakeCurrent`
"appear in no source of this build". They do — `opengl_utils.c:107` and `:110` — and a name
scan missed them only because they are string literals there and the identifiers carry an
`x` prefix. That matters beyond pedantry: a future landing that called `oglu_load_dll()` and
then `xwglCreateContext` would pass the name scan unchanged and silently restore the exact
failure the watch existed for. [THE 11-5e-1 REVIEW'S MEDIUM-1.] With the caller fact
established, `cur` is 0 on every call, `s_ctx` starts 0, and the inner `if (s_ctx)` has no
first time.

**Deleting that one branch left a SIXTEEN-function cascade with no root** — fourteen of them
still compiled in — **across sixteen files, one `*_glreset` per file**. (Two corrections in
one sentence. The commit message says *ten functions*: ten was the six names the watch itself
listed plus the four below them already in view, and the full walk found six more one level
down. The first draft of this section then said *ten files*: there are sixteen, which `grep -l`
settles in one command. History is not rewritten here, so the commit stands and this is the
correction. [The second half is the 11-5e-1 review's MEDIUM-2a.])

```
tagpu_overlay.c's context watch                  DELETED -- could never fire
  |- tagpu_overlay_glreset                       DELETED (its state went with the capture)
  |- tagpu_native_glreset            (:3619)     callerless
  |    |- tagpu_rglsl_glreset        (:3622)     reachable only through it
  |    |- tagpu_fx_glreset           (:3623)          "
  |    |- tagpu_feat_glreset         (:3624)          "
  |    |- tagpu_terr_glreset         (:3625)          "
  |    |- tagpu_shadow_glreset       (:3626)          "     [escalation reason 1]
  |    |- tagpu_hires_draw_glreset   (:3630)          "     [escalation reason 1]
  |    |     `- tagpu_hires_glreset  (tagpu_hires_draw.c:834)  one level further down
  |    |- tagpu_posebake_glreset     (:3631)          "
  |    `- tagpu_posedraw_glreset     (:3632)          "
  |         (tagpu_mark_glreset was between shadow and hires and forwarded to
  |          tagpu_text_glreset; both DELETED, their whole bodies being that forward)
  |- tagpu_scaffold_glreset                      callerless
  |- tagpu_r3d_glreset                           callerless
  |- tagpu_gui_glreset                           callerless -- KEPT, it still resets the twin
  |                                              table, the sharp layer, the cursor's
  |                                              ownership and the minimap generation
  `- tagpu_fps_glreset                           callerless
```

`tagpu_overlay_glreset` is drawn as a child of the watch because that is where it was called
from, but it is NOT one of the sixteen: it belonged to the capture in the same file and went
with it. Sixteen is the fourteen survivors plus `tagpu_mark_glreset` and `tagpu_text_glreset`.

Line numbers are `tagpu_native.c`'s at the end of this landing. **THEY WERE WRONG IN THE
FIRST WRITE-UP OF THIS SECTION — 2000 through 2011, and `tagpu_hires_draw.c:669` — and they
were wrong in exactly the way the subsection below describes**, having been read off a
masked scan whose comments had eaten their own newlines. The section refuted itself three
paragraphs apart and neither half noticed. [THE 11-5e-1 REVIEW'S MEDIUM-2, and the sharpest
thing it found: writing down a trap does not protect the page you write it on.]

**AND THEN THEY WERE WRONG AGAIN, BY ONE, FOR A COMPLETELY DIFFERENT REASON.** The corrected
numbers were read before the review round removed a stale `#include` from `tagpu_native.c:95`,
which shifted everything below it up a line — 3620 became 3619, and the rest with it. This
section had already compensated for exactly that shift in one place (the
`-Wmisleading-indentation` line, `:997` on `main` and `:996` here) and not in the diagram.
**A line number is a fact with a version**, so a page that corrects line numbers has to
re-read them after its own LAST edit rather than its first. [Round 3's MEDIUM.]

**Only the two whose entire body was a forward are deleted here.** The rest are left standing
and labelled in place — `tagpu_native_glreset` with the full banner and
`tagpu_scaffold_`, `tagpu_r3d_`, `tagpu_gui_` and `tagpu_fps_glreset` each with a line
pointing at it. (The first version of this landing labelled only `tagpu_native_glreset` and
this paragraph claimed all five, which is the labelling policy's own failure mode: a reader
grepping one of the other four found a function that looked live. The 11-5e-1 review's
MEDIUM-2d.) They stay because their modules still hold live GL state — `tagpu_shadow.c` and
`tagpu_hires_draw.c` are behind escalation reason 1 — and **a reset tree deleted ahead of the
objects it resets is worse than one that is merely unreachable**: the next landing that
deletes an atlas would have nothing left to tell it that atlas had a teardown path.

#### Two predicates that read as choices and were not

- **`s_glDrives`** (`tagpu_ftime.c`) decided *which lane prints the report*, so that exactly
  one of two live lanes did. It was set only by `tagpu_ftime_gl_begin`. With that function
  deleted it is permanently 0 and `!s_glDrives` is trivially true. The positive form is to
  delete the static and the term, and to say in the one place a reader will look why there is
  no arbitration: there is one lane.
- **`report()`'s three branches** collapsed to one. The `vk/gl` ratio needed both rings and has
  been unreachable since 4d-1; the two "one lane is silent" branches described a state the
  function can no longer be called in, because its sole caller (`tagpu_ftime_vk_sample`) pushes
  this frame's sample before it counts the frame, so `s_vkN > 0` there by construction. The
  empty case now has no branch because it has no path.

#### And one write-only pair the compiler will not catch

`s_dirty` and `s_gdirty` in `tagpu_text.c` were CONSUMED by the GL uploads that read them.
With the uploads gone they were assigned in five places (`:266`, `:300`, `:410`, `:447`,
`:474`) and read in none — and **`-Wall` does
not warn for a file-scope static that is only ever assigned**, which is the same trap
`tagpu_gui_surf.c`'s own glreset comment records. They are deleted; `s_agen` and `s_gserial`,
which tick and are never cleared, already carry the meaning for any number of consumers. That
distinction was bought by the G19f landing-2 review (both reviewers led with it independently)
and is restated beside the survivors so the next upload does not reintroduce a flag.

#### A lever that now does nothing

`tagpu_gldbg.on` had one reader left in the whole tree: `oerr`, the five-site `glGetError`
probe in `tagpu_overlay.c` — probing a context this build never creates. With it gone the file
`tagpu_gldbg.on` is inert. `research/notes/windowed-mode.md` said the lever gated two live
probes in `render_ogl.c`; it now says the lever is dead and keeps only the trap that outlives
it (GL 1.1 entry points resolve through `GetProcAddress(opengl32)`, never `wglGetProcAddress`,
which returns NULL for them under wine — calling that NULL crashes TA with the usual secondary
fault at `0x4d94e0`).

#### Verification

**Symbol table, per translation unit**, HEAD's revision compiled from a `git archive` at the
same include paths, `i686-w64-mingw32-nm --defined-only`, diffed: exactly the 13 functions and
34 statics above disappeared and **nothing else** (an earlier draft said 19 here and 34 in
the deletion list; 34 is right, and 19 is `tagpu_ftime.c`'s subtotal — the 11-5e-1 review's
LOW-9). Two entries changed shape rather than
vanishing — `_pct.part.0` became `_pct.part.0.constprop.0` and `_fprintf` became
`_fprintf.constprop.0` — which is gcc specialising a helper that now has one caller shape, not
a deletion. `tagpu_native.o` and `tagpu_gui_surf.o` show **no symbol change at all** — which rules out a
whole function having been deleted by accident, and **nothing more**. Both edits removed a
CALL from inside an existing function, and `nm` cannot see a removed call in either
direction, so the symbol table is silent on exactly the property an earlier draft of this
sentence claimed it checked. What checks that is the diff. [The 11-5e-1 review's MEDIUM.] A separate function
inventory (masked source, definitions only) agrees function for function.

**Run, on the same fixture as 11-5d**: `one-unit` on Two Continents, 1024x768, `renderer=vulkan`
confirmed in the instance's ini after the run, the arm set
`native.on='all wrecks' terr.on feat.on fx.on sfx.on mark.on order.on zoom.on vpwide.on gui.on`,
DLL md5-matched into the gamedir. Both builds run to `vk: census: … 6 pass(es) drew` (terr,
feat, unit, fx, mark, gui) with `onscreen=1`, then three captures of the X window each:

| comparison | differing pixels of 786 432 |
|---|---|
| previous build, capture 1 vs 2 vs 3 | **0** |
| this build, capture 2 vs 3 | **0** |
| **previous build vs this build** | **44 (0.01 %)** |
| …of those, outside the minimap panel | **0** |

The 44 are minimap unit blips: the two runs' AI had built different numbers of **off-screen**
units by the time each settled (`alive=14` against `alive=9`, `onscreen=1` in both), so the
dots differ and nothing in the viewport, the side panel, the resource bars or any glyph does.
The whole presented frame is otherwise identical.

The rect used to attribute those 44 is **`[0:126, 0:128]`**, and only its HEIGHT is engine
geometry: this page records the minimap as "the three 126-px surfaces" and the packet reports
`mm=106x126`, so 126 rows is the surface and **128 columns is the PANEL the surface sits in,
chosen to cover it, not a measured width**. Saying "which is the engine's own geometry" of the
whole pair was wrong [round 3's LOW], and the distinction matters where the rect IS
load-bearing — the second comparison below reports "0 outside", so 22 columns of slack could
in principle absorb a difference and report it as inside the minimap. For the 44 it cannot:
the total over the whole frame is 44, so no rect hides anything.

The first write-up said `[0:125, …]`, and that off-by-one showed up on the second comparison
below, where 4 of 48 pixels landed on row 125 and read as "outside the minimap".

**THE SAME-BUILD CONTROL IS THE PART THAT MAKES THIS A MEASUREMENT — AND IT IS NOT
SUFFICIENT ON ITS OWN.** The first attempt compared one capture from each build and reported
**28.5 %** differing; the earlier capture was taken while the scene was still settling
(`3 pass(es) drew`, greyscale, the palette and terrain pass not yet up). Two captures of ONE
build settle whether the scene is STATIC. They do not settle whether it is SETTLED, and the
two are different questions: re-measuring this landing's own review fixes produced a stable
`3 pass(es)` state whose three captures differed by 0 px and which still differed from a
6-pass capture by **202 695** pixels — the same greyscale-versus-colour gap, dressed as a
regression. **Both gates are needed: three captures at 0 px for static, and
`6 pass(es) drew` in the census for settled.**

**The lever the landing actually changed was armed and run** (this was the review's HIGH
finding, and it was right: a pixel diff is structurally incapable of covering an instrument
that writes to `tagpu.log`). `tagpu_ftime.on` on the same fixture, `renderer=vulkan`:

```
ftime: ON - GPU frame time on the Vulkan lane, two timestamps a frame, nothing blocks
ftime: vk p50 0.383 ms p99 0.620 ms (n=256/300) - one lane in this build, so no ratio: compare this against an earlier build's figure
ftime: vk p50 0.383 ms p99 0.619 ms (n=256/600) - …
ftime: vk p50 0.383 ms p99 0.618 ms (n=256/900) - …
ftime: off
ftime: ON - GPU frame time on the Vulkan lane, two timestamps a frame, nothing blocks
ftime: vk p50 0.383 ms p99 0.619 ms (n=256/300) - …
```

A FRAME-TIME FIGURE IS NOT COMPARABLE WITHOUT THE SCENE STATE BESIDE IT. A later run of the
same fixture reported `vk p50 0.163 ms` on its first report and `0.382 ms` on its second — the
first landed while the census still said `3 pass(es) drew` and the terrain pass was not up, so
it is a p50 over a cheaper frame, not a faster one. Quote the census line with the figure or
the number means nothing; this is the same settled-versus-static distinction the pixel method
needs, in the instrument's own units.

That exercises all three changed-and-still-reachable functions: `report()`'s single surviving
branch prints (`n=256/300` is `RING` full over `REPORT_FRAMES`, so the cadence is right),
`tagpu_ftime_vk_sample` accumulates, and `tagpu_ftime_poll`'s toggle path clears the ring —
the counter restarts at `/300` rather than continuing to `/1200`. Nothing leaked across the
off/on cycle **at full frame rate**; the window named in `tagpu_ftime.c`'s poll comment —
single figures to the low tens of frames a second, the swapchain's `nimg` presents inside
`POLL_MS` — is not exercised by this and is not claimed to be. Nor could the measurement have
detected a leak if one had happened: a stale pair is simply one of the 300.

`tagpu_ftime_vk_reset` also gained `s_frames = 0` in the review round, so that a lane brought
down at frame 299 of the 300-frame cadence cannot make the first sample after the next
bring-up print a "p50/p99 over 256 frames" computed from one. **That path was not exercised**:
it needs a shell-to-game lane cycle with the lever armed, and this fixture produces none. The
re-run above confirms the ordinary path still reports at `/300` and `/600` with the fix in.

**The review's own fixes were then re-measured**, both builds at `6 pass(es) drew`: three
captures each at 0 px within a build (one pair at 1 px, a minimap blip), and **48 px
cross-build of which 0 lie outside `[0:126, 0:128]`**. The one live change in that round —
`s_state` replaced by a one-shot `s_said`, and the unreachable `if (s_state != 1)` deleted —
alters no pixel.

#### Gaps this landing did not close

- **`opengl_utils.c` cannot be deleted by 11-5e at all**, and the plan's ordering had this
  backwards. It DEFINES the GL entry points every other GL file calls (`glBindTexture`,
  `glTexImage2D`, `xwglGetProcAddress`, ~50 names used by `tagpu_restoreglsl.c`,
  `tagpu_shadow.c`, `tagpu_hires*.c`, `tagpu_gaf.c`), so it goes last of everything — behind
  `tagpu_shadow.c` and `tagpu_hires_draw.c`, which are escalation reason 1.
- **Fourteen `*_glreset` functions are now unreachable and still compiled in** — five with no
  caller at all (`tagpu_fps_`, `tagpu_gui_`, `tagpu_native_`, `tagpu_r3d_`, `tagpu_scaffold_`)
  and nine reachable only through `tagpu_native_glreset`. Not a defect: each guards live GL
  state. They are 11-5e-2's and the escalation's.
- **`tagpu_hires_draw` has no caller** — the draw entry of a 104-site file. The name matters:
  an earlier draft called it `tagpu_hires_draw_frame`, which does not exist, and said the
  file's main entry was callerless. The file has two entry points and only one is:
  `tagpu_hires_draw` (`tagpu_hires_draw.c:538`) has none, while `tagpu_hires_draw_ready`
  (`:536`) IS called, from `tagpu_native.c:2777`. `tagpu_hires_draw_glreset` is reached only
  from `tagpu_native_glreset`, which is itself callerless now. For the owner, with the
  escalation — and handing an escalation a function name that does not exist is worse than
  handing it nothing. [The 11-5e-1 review's LOW.]
- **WHAT THE FIXTURE DID NOT EXERCISE**, listed because "verified by running it" is a claim
  about a particular run. `tagpu_fps.on` was never armed, so the fps readout drew nothing and
  `tagpu_vk_fps.c` never uploaded the string atlas. The STRING atlas's raster path was almost
  certainly never entered either: its only remaining consumer is `tagpu_mark_emit_text`, which
  runs only for a squad-tagged unit, and a `one-unit` fixture has none — so `tagpu_text.c`'s
  two edited `s_agen++` lines did not execute (the GLYPH half's three did, via the GUI string
  path). Both removals are provably inert, since neither flag has a remaining reader; the point
  is that the run does not cover them and the note should not read as though it did. Also not
  covered: the shell and the menus, a second resolution, `renderer=gdi` (which never enters
  `tagpu_overlay_draw` at all, so that claim in `tagpu_overlay.h` is by inspection), and **a
  display-mode change — the one event the deleted watch existed for**. The argument that it
  cannot matter is a source scan, not a run. [All from the 11-5e-1 review.]
- **`tagpu_native.c:996` still trips `-Wmisleading-indentation`**, and `tagpu_scaffold.c:366`
  and `:367` trip the same. Pre-existing and on `main`; it reads as `:997` there and moved up
  one line when this landing removed a stale `#include` above it.
- **`tools/uiwalk.py` has a wait that can never be satisfied**, and this landing explains it
  rather than causing it. `:598` detects the game's mode switch by waiting for the log line
  `GL CONTEXT CHANGED`, whose only emitter was the block deleted here — but that block was
  already unfireable on `main`, so the line has not printed since 11-2 and
  `stop_loading()` has been burning its full 150 s timeout per screen ever since. The same
  file still calls the retired `tacli glshot` (`:509`), arms `tagpu_glshot.trigger`
  (`:574`, `:614`) and waits on `tagpu_gl.ppm` (`:571`), whose producer this landing has now
  deleted outright. **Not repaired here**: uiwalk needs a different switch signal, which is
  its own piece of work and not this landing's three files. Recorded so the next person to
  run it knows why it is slow. [THE 11-5e-1 REVIEW'S LOW-10.]

#### THE TOOL BUG THIS LANDING FOUND IN ITS OWN METHOD, and what it invalidated

The comment-masking helper every reachability scan in 11-5 has used replaced **every**
character of a comment with a space — **including the newlines inside it**. A masked line
number is therefore not a source line number: a multi-line comment collapses to one line and
everything after it shifts earlier. On `tagpu_native.c` the gap is 1 621 lines
(`tagpu_native_glreset` reads as 1998 masked and is at 3619). The CALL COUNTS and the
caller/no-caller answers are unaffected — they never depended on position — so nothing 11-5a
through 11-5e-1 deleted was deleted for a wrong reason. **The line numbers quoted in prose
were affected**, and five of the seven in landing 11-5d's depth-twin chain were wrong:
`tagpu_posedraw.c:1080 → :1108`, `:793 → :814`, `:688 → :706`, `:689 → :707`,
`tagpu_vk_unit.c:2350 → :2358`. Corrected in place here and in `tagpu_posedraw.h`. The two
that were right (`tagpu_vk_unit.c:2579` and `:2587`) are exactly the two that the 11-5d review
disagreed about and that were re-read from the source to settle — which is the lesson:
**a number two readers argue over gets checked; a number nobody questions does not.**

The helper now preserves newlines, and any line number quoted from a masked scan before
2026-09-19 should be re-read from the source before it is relied on.

### 2.75 The include residue, and a type that was the last thing holding a header in — landing 11-5e-3

Three files included `opengl_utils.h` while making no GL call. The include is gone from all
three, and the count of files that include the header at all goes **nine → six**. The GL *call*
surface is unchanged at **252 / 366 over four files** (`tools/gl-sites.py`), which is the point:
this landing removes a dependency, not a call.

| file | narrow / wide | what it named from the header | action |
|---|---|---|---|
| `tagpu_fps.c` | 0 / 0 | **nothing** | include deleted |
| `tagpu_scaffold.c` | 0 / 0 | **nothing** | include deleted |
| `tagpu_render3do.c` | 0 / 0 | `GLuint`, once | spelling changed, then include deleted |
| `render_gdi.c` | 0 / 0 | `g_oglu_version` | **kept** — goes with `opengl_utils.c` |
| `tagpu_gaf.c` | 0 / **12** | `GLenum`, `GLint`, `GLsizei`, `xwglGetProcAddress`, `APIENTRY` | **kept** — goes with 11-5e-2b |

**A CALL COUNT OF ZERO DOES NOT MEAN A FILE IS FREE OF THE HEADER**, and this landing is where
that stops being a slogan. Of the five files with no GL call, only two name nothing from it at
all. `render_gdi.c` reads a *variable*; `tagpu_gaf.c` takes three types, an entry-point resolver
and a calling convention; `tagpu_render3do.c` took exactly one *type*, in one place:

```c
GLuint tagpu_r3d_atlas_rgbref(void) { return s_atlas.rgb; }
```

`GLuint` is `typedef unsigned int` (`inc/glcorearb.h:87`), `tagpu_render3do.h:14` **already**
declared the function `unsigned int`, and the field it returns is `unsigned int`
(`tagpu_gaf.h`). So the definition was the odd spelling out, the change is type-identical, and
the object file proves it: `_tagpu_r3d_atlas_rgbref` is still `T` in `tagpu_render3do.o` and
still `U` in its one caller `tagpu_posedraw.o`. **The tool that counts calls would have called
this file clean, and the include would not have compiled away** — which is why the
include test is a separate question from the call count, and is run on the RAW text (an
`#include` is itself a string literal, so a comment/string mask deletes every include).

**Two corrections to the plan's own row, which named the wrong three files.** It listed
`render_gdi.c` as residue — it is not, and the same page's "files it ends at" list says so two
paragraphs later, naming `render_gdi.c`'s `g_oglu_version` among the surviving plumbing users;
the row and the list contradicted each other. And it said 11-5e-1 had already taken
`tagpu_render3do.c`'s include — it had not; 11-5e-1 removed a `tagpu_overlay.h` include from
`tagpu_native.c`, a different file and a different header.

**Every remaining includer now has a stated reason**, which is this landing's exit condition:
`opengl_utils.c` defines the entry points; `render_gdi.c` reads `g_oglu_version`; `tagpu_gaf.c`
is 11-5e-2b; and `tagpu_hires.c`, `tagpu_shadow.c` and `tagpu_hires_draw.c` are escalation
reason 1.

#### Verification

**Build gate.** `thread-split: clean — 34 listed file(s)`, `spirv: 49 shaders, 33 programs,
headers current`, exit 0, and **zero warnings**.

**Nothing moved.** Function inventory diffed per translation unit against `main`: **0 gone, 0
added**. No engine address appears on any added line, so `exe-reverse-engineering.md` takes
nothing from this landing.

**A pre-existing warning is fixed rather than left visible.** `tagpu_scaffold.c` had four clamps
sharing two lines — correct C, which `-Wall` reads as `-Wmisleading-indentation`. It is
pre-existing, **verified by compiling `main`'s own copy of the file**, and was invisible only
because the object was cached; this landing is the first thing to rebuild that file in a while.
One clamp per line now, which is the same code.

#### Gaps this landing did not close

- **`opengl_utils.h` itself stays**, with six includers, four of them behind escalation reason 1
  (`tagpu_shadow.c` 87, `tagpu_hires_draw.c` 104, `tagpu_hires.c` 30, `opengl_utils.c` 31).
- **`tagpu_gaf.c`'s twelve wide sites** — the `getgl`/`xwglGetProcAddress` arm and the
  `glReadPixels` the refusal message names — go with the RGB mirror in **11-5e-2b**, not here.
- **`tagpu_r3d_atlas_rgbref` is still a dead-ish accessor**: it returns `s_atlas.rgb`, which
  11-5e-2 pinned to 0, and its one caller publishes `restored` from it. Retiring it is
  11-5e-2b's; this landing only corrected its spelling.

### 2.74 The GL restorer goes whole, and the predicate that pinned it was set on purpose — landing 11-5e-2

`tagpu_restoreglsl.c` is deleted entire: 39 functions, **249 of the tree's 540 masked GL call
sites**, the whole GLSL backend of the Classic++ restorer. With it go the `tagpu_gaf.c` code
that fed it, the **three** blocks that stepped it — two in `tagpu_native.c` (the in-gather step
and the `tagpu_rglsl_step_forced` block) and one in `tagpu_gui_surf.c` — and one dead accessor.
The surface goes **540 → 252** and the file count **six → four**.

**`tagpu_gaf.c` leaves the CALL set, which is not the same as being GL-free** (R2 LOW-4, and the
distinction is the reason this page reports two numbers). Its narrow count is 0 and its **wide
count is 12**: it still includes `tagpu_restoreglsl.h`, still resolves `glReadPixels` through
`wglGetProcAddress`, and still names GL entry points in the refusal `tagpu_gaf_atlas_mirror_rgb`
logs. Those 12 go with the mirror in 11-5e-2b, not here.

**The counts are reproducible now, which they were not before.** `tools/gl-sites.py` is this
landing's, and it exists because the gate's exit condition is a number that three landings had
each produced with a different throwaway script. It fixes the three things that were never
written down — the pattern, comment/string masking, and the file set — and reproduces both ends
of this landing's claim: `540 / 689` over six files on `main`, `252 / 366` over four here. It
exits 0 only when the narrow total is 0, so 11-5's exit condition can be gated on directly.

| file | before | after |
|---|---|---|
| `tagpu_restoreglsl.c` | 249 | **deleted** |
| `tagpu_gaf.c` | 39 | **0** |
| `tagpu_hires_draw.c` | 104 | 104 — escalation reason 1 |
| `tagpu_shadow.c` | 87 | 87 — escalation reason 1 |
| `opengl_utils.c` | 31 | 31 — last of all; it DEFINES the entry points |
| `tagpu_hires.c` | 30 | 30 — goes with `tagpu_hires_draw.c` |
| **total** | **540** | **252** |

#### THE FIND: the root predicate is a VALUE, and a previous landing set it deliberately

The restorer's only entry into GL was `tagpu_rglsl_job_new`, whose **one** call site is
`tagpu_gaf.c`'s `tagpu_gaf_atlas_restore`, below `if (a->restoreFailed || !a->tex || !pal)
return;`. And `a->tex` is **0 for the life of the process**:

```
a->tex = 0            tagpu_gaf_atlas_create   (the only other write is _lost's a->tex = 0)
  -> tagpu_gaf_atlas_restore returns at its third gate, every call
    -> tagpu_rglsl_job_new is never called
      -> a->job is NULL for the life of the process
        -> init_gl() never runs (its only path is job_new_x), s_glReady stays 0
          -> gl_ready() 0 -> tagpu_rcore_step returns on its second line, every frame
            -> all 249 GL sites in tagpu_restoreglsl.c are inert
```

No `glGenTextures` anywhere in the tree fills `a->tex`, and there is no `&a->tex`. **Landing
11-4c took the one that did** — on 2026-09-19, hours before this landing — and said so in the
comment it left behind: *"`a->tex` is set to 0 rather than left alone so that a re-created
atlas cannot carry a stale name, and it is 0 for the life of the process: the `glGenTextures`
that used to fill it went in landing 11-4c."*

**That is COROLLARY 2, not corollary 3.** The pin is a value, set on purpose, and 11-4c's
justification reasons entirely about stale texture names — it does not mention that the same
line put 288 GL call sites and one live feed out of reach. The cost of a deliberate pin hides
in its justification, exactly as 11-5d said. A second pin sits underneath (nothing calls
`oglu_load_dll`, so every `getgl` is NULL) and would stop the same tree on its own, but it is
not the root, and a write-up that named it as the root would have sent the next reader to the
wrong file.

#### AND UNPINNING IT RAN A CONSUMER THAT HAD NEVER RUN — corollary 1, measured

`atlas_paint` ended in `if (a->job) restore_enqueue(a, e);`. `restore_enqueue` does two things:
it queues the frame on the GL job **and** appends it to the **published frame list** — which is
the *Vulkan* lane's restore input and has nothing to do with GL. So the GL predicate was gating
the other backend's feed, and since 11-4c it has been gating it shut.

Measured with a temporary probe in `atlas_paint`, pre-landing build, `fx-mix`, `restorevk.on`:

```
PROBE feat: PAINT AFTER ARM n=15 job=NULL tex=0 rlistN=0 (this paint will NOT enqueue)
PROBE fx:   PAINT AFTER ARM n=3  job=NULL tex=0 rlistN=0 (this paint will NOT enqueue)
vk: feat: restoring the atlas HERE - 0 of 0 frames over 2048x2048, generation 1
vk: fx:   restoring the atlas HERE - 0 of 0 frames over 2048x2048, generation 1
queue drains: 0
```

The list is armed (`rlistWant` 1, `rlist` allocated), the atlas holds 15 entries, and not one
reaches the consumer. The same probe on the landing's build:

```
PROBE2 fx: paint n=3 rlistWant=1 rlist=SET rlistN=0 cap=256 minEdge=0 w=12 h=9
PROBE2 fx: paint n=3 rlistWant=1 rlist=SET rlistN=1 cap=256 minEdge=0 w=7  h=4
vk: feat: restoring the atlas HERE - 15 of 15 frames over 2048x2048, generation 1
vk: fx:   restoring the atlas HERE - 5 of 5 frames over 2048x2048, generation 1
queue drains: 162
```

**What it costs when it is shut.** `tagpu_gaf_atlas_restore_vk` seeds the list once from the
entries present at arm time; `rlist_restart` — the only re-seed — is reachable only from that
arm and from an overflow inside `rlist_add`, which cannot happen if `rlist_add` never runs. And
`job_clear_dest` (a recycle, a repack) calls `rlist_reset`, which sets `rlistN = 0`. So the list
could only shrink: seeded once, emptied by the first reset, never refilled. Every frame inserted
afterwards stays indexed for the session.

#### THE ONE-LINE FIX WAS NOT SAFE, AND THE REVIEW CAUGHT IT BEFORE IT LANDED

The change above — `if (a->job) restore_enqueue(a, e);` → `restore_enqueue(a, e);` — reads as the
removal of a guard, and it was written up here as one. **It is a use-after-free**, and the
landing's second reviewer found it. Every link was then checked against the source:

| link | evidence |
|---|---|
| the published pointer is RAW | `tagpu_render3do.c` `tagpu_r3d_atlas_restore_list` ends `return s_atlas.rlist;` |
| it is captured at publish | `tagpu_posedraw.c` `pd_view_publish`: `s_pub.restoreFrames = fr;` |
| on the FIRST window of the frame | its one caller, `pd_begin`: `int first = (s_win++ == 0); … if (first && s_recording)` |
| the build ghost paints AFTER that | `tagpu_native.c`: `tagpu_posedraw_begin(&s_pv)` … then `ghost_record(…)`, whose comment reads *"in its own window after the units'… It is AFTER on purpose"* |
| and a ghost can reach the paint | `tagpu_r3d_atlas_uv` → `atlas_get` → `tagpu_gaf_atlas_get`, which inserts and paints on a miss |
| where the buffer moves | `restore_enqueue` → `rlist_add` → `rlist_room` → `realloc` — or, out of memory, `free` |

So on any frame where a build ghost needs a texture not yet atlased, the unit atlas's list can be
reallocated **after** its address was published, and the render thread dereferences the old one
in the same iteration. The feature and effects atlases escape only because their publication
happens to be the last write of their frame; the unit atlas does not, and nothing enforces it.

**The feed is therefore NOT restored by this landing.** Not calling `restore_enqueue` is exactly
today's behaviour — `a->job` has been NULL for the life of the process — so the deletion stays
behaviour-preserving, and the defect is documented rather than half-fixed. Opening a live
data path is a different landing from deleting a backend, and it needs its own measurement and
its own review: it is **11-5e-2c**, and the code carries the whole argument at `atlas_paint`.

**What the deletion leaves behind is stronger than what it found.** `restore_enqueue` and
`rlist_add` were callerless once the backend went, so both are deleted too — and with them the
only path by which a published buffer could move:

> `rlist_room` (the `realloc`/`free`) is now reachable only from `rlist_restart`, and
> `rlist_restart` only from `tagpu_gaf_atlas_restore_vk`, which latches on `a->rlistWant` and
> runs **once per atlas per session**, before anything has published a non-NULL pointer. So
> `a->rlist` is assigned exactly once and is neither moved nor freed for the life of the atlas.

That is a lifetime the code enforces. Before this landing the same property held, but it rested
on `a->job` being NULL for a reason stated three files away — which is precisely the kind of
predicate THE NAMED RULE is about.

**And there is a second pin underneath, which 11-5e-2c must clear as well.** Feeding the list is
not sufficient to make the other lane's restore *visible*: `restored` is published by three
producers and all three are gated on `s_atlas.rgb`, which has exactly one write left in the tree
(`a->rgb = 0` in `tagpu_gaf_atlas_lost`) —

| producer | expression |
|---|---|
| `tagpu_feat.c` | `s_pub.restored = (s_atlas.rgb && tagpu_classicpp_assets())` |
| `tagpu_fx.c` | `s_pub.restored = (s_atlas.rgb && tagpu_classicpp_assets())` |
| `tagpu_posedraw.c` | `s_pub.restored = (tagpu_r3d_atlas_rgbref() && …)`, and that accessor returns `s_atlas.rgb` |

— so the measurement above shows the **restorer** running (15 of 15, 5 of 5, 162 drains) and says
nothing about the drawing passes, which take their "no restore" arm before they look at the list.
That is consistent with the pixel A/B below finding no change outside the minimap, and it is why
that A/B is not evidence either way. `tagpu_terr.c` also publishes `restored`, from its own
fixed-list restore; it is not gated on this field and is the one that is actually live.

**A third thing is armed and waiting for whoever does 11-5e-2c**: `rgbAniso` also lost its only
writer here, so it reports 0 while `tagpu_vk_unit.c` stands the whole frame down on
`h.atlasRgbAniso != s_twinAniso` — and `s_twinAniso` is `tagpu_classicpp_light()->aniso`, which
**defaults to 4**. The test is unreachable only while `restored` is 0. Unpin one without the
other and the unit pass draws nothing. All three are labelled at their declarations in
`tagpu_gaf.h`.

**Why no pixel A/B could have caught it.** 11-4c's gate was `0 px of 786432 across all 64
cross-build pairs`, and it was honestly run. The feed only exists when `tagpu_restorevk.on` is
armed, and that fixture did not arm it. This is 11-5e-1's HIGH generalised: *a pixel diff cannot
cover a lever the fixture does not arm* — there it was an instrument writing to `tagpu.log`,
here a lever file.

#### What was deleted, and how each was shown to be unreachable

| deleted | why it could not run |
|---|---|
| `tagpu_restoreglsl.c` (39 functions) | `gl_ready()` is 0; `tagpu_rcore_step` returns on its second line |
| `tagpu_gaf_atlas_restore` | returns at `!a->tex`, every call |
| `twin_mips` | only caller was inside that function's `a->job` branch |
| `dump_if_armed` | same, plus it reads `a->tex` and `a->rgb`, both pinned 0 |
| `tagpu_gl_rgba_readback`, `rgb_rows` | only callers were `mirror_rgb_step`'s unreachable half |
| `tagpu_r3d_atlas_texref` | **zero call sites**, and it returned the pinned `s_atlas.tex` |
| `restore_enqueue`, `rlist_add` | callerless once the backend went — see the review subsection: deleting them is what makes `a->rlist`'s address constant by construction |

Inventory diffed per translation unit against `HEAD`: **47 functions gone, nothing else taken,
nothing new**. Every deleted non-static is absent from every object file; `tagpu_rglsl_tileable`
— which lives in `tagpu_restore_core.c`, not in the deleted backend, and is called from
`tagpu_terr.c:961` and `tagpu_gaf.c` — still links.

#### What was REDUCED rather than deleted, and why

`tagpu_gaf_atlas_mirror_rgb_step` keeps the branch that was already the only reachable one
(`!a->rgb`, and `a->rgb` had exactly one non-zero writer, the deleted `tagpu_gaf_atlas_restore`).
The rest of the RGB read-back — `mirrorRgb*`, the `atlasRgb*` publications and their consumers —
is **11-5e-2b**: it reaches **111 sites across `tagpu_vk_feat.c`, `tagpu_vk_fx.c`,
`tagpu_vk_gui.c`, `tagpu_vk_unit.c`, `tagpu_vk_terr.c`, `tagpu_terr.c` and the four producers**,
and the terrain's use of it is a different path that this landing did not trace. A consumer tree
deleted ahead of its consumers is worse than one that is merely unreachable — the same reason
11-5e-1 left fourteen `*_glreset` functions standing.

Consequently `a->rgb` is now 0 for the life of the process, which pins **all three** producers of
`restored` (`tagpu_feat.c`, `tagpu_fx.c`, `tagpu_posedraw.c` — the table in the review subsection
above), and with them `dumpedN`, `pal`, `palSerial` and `rgbAniso`, none of which has a writer or
a reader left in the tree. `pal` was already write-only before this landing: the tileability test
it is named for takes its palette from its caller. All are labelled `[PINNED]` at their
declarations in `tagpu_gaf.h` and none is acted on here — the struct's restore half unwinds as
one piece in 11-5e-2b, and a field deleted ahead of that is one whose consumers get found by the
compiler one at a time.

#### Verification

**The GL-site count, reproducibly.** `tools/gl-sites.py` (new here) on this tree:
`252 / 366 · 4 file(s)`; on `main`, `540 / 689 · 6 file(s)`. Both match what this section claims.

**Build gate.** `thread-split: clean — 34 listed file(s)`, `spirv: 49 shaders, 33 programs,
headers current`, exit 0, no new warnings. Editing `tools/spirv-gen.py` re-hashes the freshness
chain, so 13 `inc/spirv/*.spv.h` are regenerated: **one `transform` provenance line each and
zero SPIR-V payload words changed**, verified by diff. The SPIR-V gate is untouched because
`tools/spirv-gen.py:extract_restore()` reads the five restore shaders out of
`tagpu_restore_glsl.h` — the header, note the underscore — and not out of the deleted `.c`;
`tagpu_restore_glsl.h` also feeds `tools/tascene`'s browser pack and is **not** part of the GL
lane's fate.

**Verified by running it — RE-MEASURED after the review's fix.** Instance `e5a`, `one-unit`,
Two Continents, 1024x768, `renderer=vulkan`, a private display, three full runs, each settled to
a steady `vk: census: … 2 pass(es) drew (terr=1 unit=1)`:

| comparison | differing pixels of 786 432 |
|---|---|
| within one run, three grabs, all three runs (nine pairs) | **0** — the scene is STATIC |
| **the same build, two separate RUNS** — the noise floor | **44**, every one inside the minimap |
| **`main`'s build vs this build** | **44** — and **0 outside the minimap rect** |

**THE CROSS-BUILD FIGURE IS THE NOISE FLOOR, TO THE PIXEL**, and not one pixel outside the
minimap differs. The minimap is where both figures live: its blobs are greyscale in one run and
player-coloured in another, at different positions, so it varies run to run on its own. This is
the method finding the gate keeps re-learning — three grabs inside one run prove the scene is
static and bound NOTHING about run-to-run variation; that is a different question needing a
different control. 11-5e-1 reported "44 px, 0 outside" without that control and so had no bound
on its own noise.

**AND THE LOG IS THE DIRECT EVIDENCE, not the pixels.** The claim is now *behaviour-preserving*,
so the lines that could have changed are the reading that matters — and they are byte-identical
between `main`'s `ddraw.dll` and this one:

```
fx:   restorevk -- ... (0 entries seeded, 8192-frame bound)
feat: restorevk -- ... (0 entries seeded, 16384-frame bound)
unit: restorevk -- ... (25 entries seeded, 8192-frame bound)
vk: unit: restoring the twin HERE - 25 of 25 frames over 2048x2048, generation 1
```

That is what "the feed stays shut" means at run time: the published list is seeded once and never
fed, exactly as before the landing. The pixel A/B is blind to this half by construction — the
earlier probe runs are its measurement — which is *a pixel diff cannot cover a lever the fixture
does not arm*, stated once more.

**THE FIXTURE ITSELF COST MORE THAN THE MEASUREMENT, and the cause is worth writing down.** Three
runs failed before these three, all identically: the game loaded our DLL, wrote its ~18 hook and
patch log lines, and then stopped at window creation on TA's modal *"Error: Environment
Initialization Failed! Check your DirectX setup"* with no `ErrorLog.txt`. **It reproduced with
`main`'s DLL, which is the tell that it is not the build under test.** The cause is
`tools/tacli`: `--display` is honoured only by `tacli create`, because `_launch` builds its
environment from `Instance.env()`, which reads `meta["display"]` — and nothing persists the flag
into the meta. The instances were pinned to a display left behind by a parallel session, which
had since died, so every launch was aiming at a dead X server. A new instance created with
`tacli create --display <live>` is the way through; editing the stored display is not, because
`instance.json` is shared state another session may own.

#### Gaps this landing did not close

- **`tagpu_shadow.c` (87) and `tagpu_hires_draw.c` (104)**, with `tagpu_hires.c` (30) behind
  them, and `opengl_utils.c` (31) last of all — **escalation reason 1**, unchanged.
- **11-5e-2b**: the RGB read-back, the `mirrorRgb*` fields, the `atlasRgb*` publications, the
  `restored` flag in `tagpu_posedraw.c`, and the refusal messages that read as driver faults
  (`gui: no glReadPixels/FBO entry points ...` fires on a perfectly healthy run).
- **The restore dump oracle is now one-sided for the sprite and unit atlases**, exactly as
  landing 11-5c left the terrain's: `tagpu_restore_<tag>.rgba` was written by `dump_if_armed`
  and is not written by anything now, while `tagpu_restore_<tag>_vk.rgba` still is. It had
  already stopped being written when 11-4c pinned `a->tex`; the skill still described the pair
  as two-sided and is corrected with this landing.
- **How much the restored feed actually paints is not a stable number.** The probe run reached
  162 queue drains; an unprobed run of the same build, settled 120 s, reached 1 (the unit
  atlas's 25-entry seed). The enqueue path is proven to run post-landing and proven not to run
  pre-landing; the quantity depends on how many frames the scene inserts after the arm, and
  this fixture varies a great deal between launches (the feature atlas held 15 entries in one
  run and 1162 in another). A fixture that pins that down is what 11-5e-2b needs.
- **11-5e-2c — the restore feed, which this landing proved broken and did NOT fix.** The
  one-line change is a use-after-free (the review subsection above). Restoring the feed needs,
  in this order: a **bound** — allocate `rlist_cap(a)` frames once in the arm and make
  `rlist_room` a pure bounds test, 0.36 MB for the unit atlas and 0.72 MB for feat, against a
  read-back mirror this file already declines to free at 16 MB; an **ordering** for
  `rlist_restart`, which rewrites the array in place and so is not covered by the bound — take
  the unit list in `tagpu_posedraw_handover`, after every paint of the frame, rather than at
  `pd_begin`; and then the two pins above, `restored` and `rgbAniso`, or the unit pass stands
  down on every frame that would have sampled the restored atlas.
- **No engine address was touched or read**: the diff's added lines carry no `0x4…`/`0x5…`
  constant, so `exe-reverse-engineering.md` takes nothing from this landing.

### 2.76 The restored twin's read-back, and the arm that was quietly doing two jobs — landing 11-5e-2b (part 1)

The Classic++ restored twin reached a Vulkan pass two ways: as **texels**, read back off the GL
twin with `glReadPixels` and handed over as `atlasRgb`; or as the **frame list**, which the other
lane paints into its own twin on the device. 11-5e-2 established that the first of those cannot
happen in any process. This landing removes it — the consumers, the publications, the hand-over
fields and the arm — for the unit, feature, effects and terrain atlases. The GUI atlas keeps its
mirror, for a reason given below that is not "it was harder".

#### THE PREMISE, re-derived here rather than inherited

Every link re-checked against the tree at `67271d9`, with a comment/string mask so a mention in
prose does not read as a call:

| link | evidence |
|---|---|
| `opengl32.dll` is never loaded | `oglu_load_dll` has **no caller** — the only two occurrences are its own definition (`opengl_utils.c:100`) and its declaration (`opengl_utils.h:36`), and it holds the tree's only `LoadLibraryA("opengl32.dll")` |
| so every GL entry point is NULL | `getgl` is `GetModuleHandleA("opengl32.dll")` then `GetProcAddress`; with the module absent it resolves nothing |
| so the mirror is never armed | `tagpu_gaf_atlas_mirror_rgb` returns at `if (!x_glReadPixels \|\| !glGenFramebuffers \|\| …)` — **before** its own `calloc`, so `a->mirrorRgb` stays NULL for the life of the process |
| and its row count is 0 independently | `a->mirrorRgbRows` has **four** writers in `tagpu_gaf.c` (`:532, :655, :707, :771` at this landing's tip) and every one assigns 0. There is no non-zero writer in the tree |
| so every publish guard is false | `tagpu_feat.c` and `tagpu_fx.c` both gate on `if (s_atlas.mirrorRgb && s_atlas.mirrorRgbRows > 0)`; `tagpu_posedraw.c` took it from an accessor that returns NULL on `!s_atlas.mirrorRgb` |
| so `atlasRgb` was NULL and `atlasRgbRows` 0 | on **every** published frame of **every** atlas, since 11-4c pinned the GL bring-up |

#### WHAT WENT

`atlasRgb*` at **112 code sites** on `main`, **29** after — counted with the mask above, so
tombstone prose does not inflate either figure:

| file | main | after | what left |
|---|---|---|---|
| `tagpu_vk_unit.c` | 19 | 2 | `rgb_stage_bytes`, `rgb_rows_due`, `atlas_rgb_upload` whole; the staging share they reserved; `s_arSerial`/`s_arReq` |
| `tagpu_vk_terr.c` | 14 | 0 | the `shared_resize` + `memcpy` upload path 11-5c left behind when it took the producer |
| `tagpu_vk_feat.c` | 9 | 0 | `doRgb`, `rrows`, `rbytes`, the second half of the atlas staging, the `rgb_only:` label |
| `tagpu_vk_fx.c` | 9 | 0 | the same shape |
| `tagpu_posedraw.c` | 10 | 1 | the arm, the step, the accessor call, the either/or zeroing |
| `tagpu_feat.c` / `tagpu_fx.c` | 6 / 6 | 0 / 0 | the arm, the step, `s_mirrorRgbAsked`, the publish block |
| `tagpu_posedraw.h` | 5 | 1 | `atlasRgb`, `atlasRgbRows`, `atlasRgbMips`, `atlasRgbSerial` |
| `tagpu_feat.h` / `tagpu_fx.h` / `tagpu_terr.h` | 3 each | 0 | the same three fields each |
| `tagpu_vk_gui.c` / `tagpu_gui_surf.c` / `tagpu_gui.h` | 16 / 6 / 3 | unchanged | **the GUI, deliberately** |

The 29 that remain are **25 GUI** and **4 `atlasRgbAniso`**, counted at this docs commit —
**this landing's own review then added a fifth `atlasRgbAniso` site** (`s_pub.atlasRgbAniso =
0.0f;` in `tagpu_posedraw.c`'s else branch, its latent-clarity finding), so the figure on `main`
is **30**, and §2.78 carries the count forward from there. On the producer side,
`mirrorRgb`-or-`mirror_rgb` as a SUBSTRING — both spellings, so the `tagpu_*_mirror_rgb*`
function names and `s_mirrorRgbAsked` are counted and not only the struct fields — goes
**102 → 57**: `tagpu_render3do.c` loses 16, `tagpu_fx.c` 12, `tagpu_feat.c` 11 and
`tagpu_posedraw.c` and `tagpu_render3do.h` 3 each, and what is left is `tagpu_gaf.c` (40) +
`tagpu_gaf.h` (10) held alive by `tagpu_gui_surf.c`'s **7**. That is the whole remaining mirror,
and it now has exactly one consumer. **The first draft of this line said 68 → 51 and labelled it
`mirrorRgb*`/`mirror_rgb*`** — those are the figures for a pattern anchored with `\b`, which
matches neither spelling inside an identifier, so the label named a surface the number did not
count. `atlasRgb` is unaffected: it reads 112 → 29 under either pattern, because nothing embeds
it after a word character. [The 11-5e-2b landing review, finding 4.]

Also gone: **`mirroredMippedN`**, which had two writers in `tagpu_gaf.c` and no reader anywhere —
`tagpu_gaf.h` had already written down that it goes with this landing.

#### THE NEAR MISS: an arm named for one thing was arming two

`tagpu_r3d_atlas_mirror_rgb_want` reads as the read-back's arm and is named as one. It is also
**the only caller of `tagpu_gaf_atlas_restore_vk` for the unit atlas** — the feature and effects
atlases arm their own lists in `tagpu_feat.c:267` and `tagpu_fx.c:173` (at this landing's tip),
and the unit atlas's arm
lives inside this function, three lines above the mirror's:

```c
if (!s_rlistAsked) s_rlistAsked = tagpu_gaf_atlas_restore_vk(&s_atlas);
if (!s_rlistAsked && !s_mirrorRgbAsked)
    s_mirrorRgbAsked = tagpu_gaf_atlas_mirror_rgb(&s_atlas);
```

Removing the call along with the read-back it is named for leaves the unit atlas with **no list
armed**, so `tagpu_vk_unit.c`'s `restore_want` never makes a job, so the twin is never painted,
so every restored frame stands down — silently, because a lane with no list stands down rather
than complains. It survives as `tagpu_r3d_atlas_restore_want`, which arms the list and nothing
else; `_mirror_rgb_step` and the `_mirror_rgb` accessor are gone.

**This is THE NAMED RULE's mirror image and it belongs beside it.** The rule is about predicates
that quietly encode "the backend is ready" as "the work is possible". This is a *name* that
quietly encodes two jobs as one, and the deletion pass reads names. The check that catches it is
the same one either way: before removing a call, ask what else it does — not what it is called.

#### WHAT STAYED, AND WHY EACH ONE

- **`atlasRgbAniso`.** It is the ratio the *other* lane's twin is filtered at, a property of that
  sampler rather than of the mirror, and the list accessor publishes it on the list path for
  exactly that reason. `tagpu_vk_unit.c` stands a frame down on
  `h.atlasRgbAniso != s_twinAniso`, which is the only thing between a restored frame and
  differently filtered art. It is the field 11-5e-2 labelled `[PINNED 0]`, and unpinning it is
  11-5e-2c's, not this landing's.
- **`s_arImg` / `s_arView` / `s_arHave` and the whole published-list path** in all four passes.
  The restorer paints that image on the device and, after this landing, is its **only** writer —
  which is a smaller surface than it had, not a larger one.
- **The GUI atlas's mirror**, and the reason is a protocol rather than a behaviour. Its colour
  twins are already unreachable through **three independent pins**: `s_colValid`
  (`tagpu_gui_surf.c:161`, declared `= 0`, no other writer) makes `colourTwins` always 0;
  `twin_sprite` and `twin_copy` both `return 0` **unconditionally**, so no op ever carries
  `TAGPU_GUICOL_ON` and `tagpu_vk_gui.c` tests a bit nothing sets; and `atlasRgb` is NULL for
  the same `glReadPixels` reason as everywhere else. Removing it would therefore change no
  behaviour — but `tagpu_gui_surf.c` never calls `tagpu_gaf_atlas_restore_vk`, so
  `tagpu_vk_gui.c` has **no list path to fall back to** and 16 live sites reading the fields.
  The hand-over's SHAPE moves for a consumer that still has working code, and afterwards the UI
  cannot express a colour twin until someone designs a route. That is a **protocol change**, in
  the words `tagpu_gui_surf.c:150` has used since 11-4b, and it is a separate landing.

#### MEASURED

Nine runs on instance `e5a`, scenario `one-unit` at 1024×768 with `restorevk.on` and
`classicpp.on` armed, three grabs four seconds apart in each: **five of `main`'s `ddraw.dll`**
(`--keep-dll`, so `tacli` does not overwrite it) and **four of this branch's**, at `--maxfps 0`
and again at `--maxfps 30`.

| comparison | differing pixels of 786 432 |
|---|---|
| within one run — three grabs, all nine runs (27 pairs) | **0** — the scene is STATIC |
| **`main`'s build against itself, two separate RUNS** — the noise floor | **48**, every one inside the minimap |
| `main`'s build vs this build — all **20** cross-build pairs | **48 or 0**, and **0 outside the minimap in every one** |
| four of those 20 — `main`'s fifth run against each branch run | **0 over the whole frame**, byte-identical PNGs |

**THE FIXTURE IS BIMODAL AND THE BUILD DOES NOT PICK THE MODE.** The nine runs land in one of
exactly two images, 48 px apart. Four of `main`'s five runs produced state A and the fifth
produced state B — the same state all four branch runs produced, **byte for byte**. A build that
produces the other build's output exactly is the strongest form this comparison can take, and it
is only available because the control was run five times instead of once.

**What the 48 px are**: one unit's dot on the minimap, at a different map position. `units:
alive=12..16 onscreen=1` in every run — the unit is off the camera, so it exists only on the
minimap, which is why **nothing outside that rect moves in any of the twenty pairs**. And the
dots are not ours to draw: `tagpu_gui_surf.c`'s minimap rule compares the engine's `+0x142DB`
composite against its `+0x142DF` fog base and passes the ENGINE's pixel through wherever they
differ, so the unit dots, the radar arcs and DrawPoint's points all arrive as engine pixels. A
landing that writes no engine memory cannot move one.

**A BATCHED CONTROL CANNOT SEPARATE THE BUILD FROM DRIFT.** The first six runs were three of the
branch's build and then three of `main`'s, and they came out three-and-three on the two states —
which reads exactly like a build difference and is not one. Consecutive runs land in the same
state because whatever drifts is slow. 11-5e-2 learned that three grabs inside one run bound
nothing about run-to-run variation; the same sentence one level up is that **a run of N controls
taken back-to-back bounds nothing about drift across the session**. Interleave the control with
the subject, or run enough of it to see the mode flip. Here it took a fifth run of `main` to see
it, and the pacing cap that was supposed to test frame timing is what produced that fifth run.

**AND THE LOG IS THE DIRECT EVIDENCE, not the pixels**, because the claim is that the deleted
path never ran. All nine runs produce the identical set of restore lines — the same multiset in
every one, with only the order of the `fx`/`feat` arms varying, and that ordering varies inside
`main`'s runs too:

```
terr: restorevk -- the restored atlas is the other lane's to paint, so no read-back …
fx:   restorevk -- … (0 entries seeded, 8192-frame bound)
feat: restorevk -- … (0 entries seeded, 16384-frame bound)
unit: restorevk -- … (25 entries seeded, 8192-frame bound)
restorevk: unit: lazy restore armed (2048x2048 twin of the 2048x2048 atlas)
restorevk: unit: the twin's 2 mip level(s) are reduced HERE, by the same integer (sum+1)/4 …
vk: unit: restoring the twin HERE - 25 of 25 frames over 2048x2048, generation 1
```

**That last pair of lines is what the near miss above would have deleted.** `unit: restorevk --
… (25 entries seeded …)` is `tagpu_gaf_atlas_restore_vk` latching, and it is reached only
through the renamed arm; `vk: unit: restoring the twin HERE - 25 of 25` is the consumer finding
the list. Had the arm gone with the read-back, both lines would simply be absent and every other
line — and every pixel, since the restored unit is one commander at ordinary zoom — would have
looked the same. The oracle for this landing is a log line, not a picture.

The only cross-build difference anywhere in the logs is the terrain restorer's own timing line
(`134 of 139 frames = 3408 ms … GPU 1599 ms` against `133 of 138 = 3371 ms … 1589 ms`), which is
wall clock, and which varies between runs of `main` by more than it varies between the builds.

**Build:** `ddraw.dll` clean, `thread-split: clean — 34 listed file(s)`, `spirv: 49 shaders, 33
programs, headers current`. The DLL is **1 550 336 bytes against `main`'s 1 558 016** — 7 680
smaller. **GL call surface unchanged at 252 narrow / 323 wide** (`tools/gl-sites.py`): this
landing removes no GL call, only the plumbing above one. (The wide figure read 366 when this
section was written; §2.77 below is why it does not now, and the narrow one is untouched.)

#### Gaps this landing did not close

- **`tagpu_shadow.c` (87), `tagpu_hires_draw.c` (104), `tagpu_hires.c` (30), `opengl_utils.c`
  (31)** — **escalation reason 1**, unchanged. They are why gate 11-5e cannot finish on its own.
- **The GUI's protocol change**, above: `colourTwins`, `s_colValid`, the two unconditional
  `return 0`s, and `atlasRgb*` out of `tagpu_gui.h`'s hand-over. It is the last consumer of the
  RGB mirror, and until it goes so do `tagpu_gaf.c`'s 40 sites, `tagpu_gaf.h`'s 10, and
  **`tagpu_gaf.c`'s ONE remaining GL site** — `xwglGetProcAddress` inside `getgl`, which is the
  only part of 11-5e-2b that moves the gate's own count. This read "12 wide GL sites" until
  §2.77 found that eleven of the twelve were `glog`, this file's own logger.
- **The producer half proper** — `tagpu_gaf_atlas_mirror_rgb`, `_step`, the `mirrorRgb*` fields
  and `tagpu_r3d_atlas_mirror_rgb`'s remaining siblings — waits on the GUI for the same reason.
- **`tools/spirv-gen.py` raises `NameError: name 'die' is not defined`** when
  `unditherer/models/*.w32.bin` is absent: `die` is defined below its first use in
  `restore_kmax`. It surfaced building `main`'s DLL from a tree without them (they are tracked,
  so a full checkout or `git archive` has them; the DLL's build also embeds them,
  [compute-restorer](../compute-restorer.html), *A model per job*) and turns a clear diagnostic into a
  traceback. Not this landing's file; recorded
  so the next person building outside a full checkout does not re-diagnose it.
- **No engine address was touched or read**: the diff's added lines carry no `0x4…`/`0x5…`
  constant, so `exe-reverse-engineering.md` takes nothing from this landing.

### 2.77 The gate's own tally was counting this fork's logger — landing 11-5e-2b (the measure)

`tools/gl-sites.py` was committed by 11-5e-2 so that gate 11-5's exit condition could be gated on
directly instead of re-derived by each landing, on the principle that *an exit condition nobody
else can reproduce is an assertion*. It reproduces — and one of its two columns was wrong.

**THE DEFECT IS ONE CHARACTER CLASS.** The two patterns were:

```python
NARROW = re.compile(r"\b(?:gl|x_gl)[A-Z][A-Za-z0-9]*\s*\(")
WIDE   = re.compile(r"\b(?:gl|x_gl|oglu_|wgl|xwgl)[A-Za-z_][A-Za-z0-9_]*\s*\(")
```

WIDE is meant to be NARROW plus the three families that are GL without being spelled
`glSomething` — `oglu_*`, `wgl*`, `xwgl*`. But it relaxed `[A-Z]` to `[A-Za-z_]` for the **bare
`gl` prefix as well**, so `gl` followed by a lower-case letter matched, and three of this fork's
own identifiers walked in:

| token | what it actually is | calls |
|---|---|---|
| `glog` | the per-module logger — `fopen("tagpu.log", "a")`, four lines, in `tagpu_gaf.c:27` and `tagpu_gui_hook.c:103` | **32** |
| `glyph_obj`, `glyph_raster`, `glyph_block_capture`, `glyph_block_mark` | the text pass's glyph bitmap helpers | **9** |
| `gl_probe` | a local helper in `tagpu_hires.c` | **2** |

**WHAT IT COST, and it was not cosmetic.** Three files were tracked as carrying GL while carrying
none of it, and one of them was load-bearing for this gate's plan:

| file | was | is | what the difference was |
|---|---|---|---|
| `tagpu_gaf.c` | 0 / **12** | 0 / **1** | 11 × `glog`. The one real site is `xwglGetProcAddress` inside `getgl` |
| `tagpu_gui_hook.c` | 0 / **25** | — | 21 × `glog`, 4 × `glyph_block_*`. It has no GL at all and now does not appear |
| `tagpu_text.c` | 0 / **5** | — | 3 × `glyph_obj`, 2 × `glyph_raster`. Likewise |
| `tagpu_hires.c` | 30 / 33 | 30 / **31** | 2 × `gl_probe` |
| **TOTAL** | 252 / **366** | 252 / **323** | |

**THE NARROW COLUMN — the one the exit condition is stated on — IS UNAFFECTED**, at 252 over the
same four files, because it always required the capital. Nothing about gate 11-5's exit moves.

**THE CLAIM THIS FALSIFIES IS THIS GATE'S OWN.** The plan and the roadmap both said
`tagpu_gaf.c` "is not GL-free: 0 narrow, **12 wide**", and that those 12 "go with the RGB mirror
in 11-5e-2b". Eleven of them are the logger and were never GL. **One** goes with the mirror, and
`tagpu_gaf.c` reaches 0 when it does. Corrected in both notes.

**Every wide figure written in these notes before this landing is overstated by the same three
tokens** — 43 across the tree. The narrow figures are all correct as written. Rather than restate
six historical rows measured on the day with the old pattern, the correction is recorded once,
here, and the forward-looking claims are fixed in place.

**Why this is worth a landing of its own.** A tool committed so that a gate can be gated on is
only worth that if its numbers are right; and this is the second time this gate has been bitten
by a pattern rather than by the code. 11-5e-1 counted `glyph_raster` and `glreset` into a *689*
that should have been 592 — the same family of false positive, caught then by comparing two
patterns and fixed by narrowing one of them. The lesson the first time was "name the pattern".
The lesson this time is **look at what the pattern matched**: `tools/gl-sites.py --list <file>`
prints every site it counts, and one run of it against `tagpu_gaf.c` would have shown eleven
`glog` calls at any point in the last three landings.

### 2.78 The UI's read-back, the producer behind it, and a file that goes GL-free on both counts — landing 11-5e-2b (part 2)

§2.76 took the restored twin's `glReadPixels` mirror out of the four **world** atlases, each of
which had a live published-list replacement to fall back on. It left the **UI** atlas standing
and said so in terms: that one has no list, so removing its mirror moves a protocol rather than
a feature, and it deserved its own landing. This is that landing, and it takes the producer and
`tagpu_gaf.c`'s last GL call with it.

#### THREE PINS, NOT ONE — why nothing observable changes

The world lanes stood down for one reason (`atlasRgb` was NULL). The UI lane stands down for
**three independent** ones, each re-checked against the tree at `c76b56d`:

| pin | evidence |
|---|---|
| `colourTwins` is always 0 | `s_colValid` is declared `static int s_colValid = 0;` at `tagpu_gui_surf.c:161` and that declaration is its **only writer** — the file's other nine occurrences are one read (`:2007`, `s_mHand.colourTwins = s_colValid ? 1 : 0;`), one counter dump (`:2213`) and seven lines of prose |
| no op ever carries `TAGPU_GUICOL_ON` | `twin_sprite` (`:715`) and `twin_copy` (`:862`) both `return 0` **unconditionally** — since 11-4b, when the terms they computed (`t->rgb`, `s_atlas.rgb`) became GL object names that no longer exist. So `tagpu_vk_gui.c` has been testing a bit nothing sets |
| `atlasRgb` was NULL on every frame | §2.76's premise chain, unchanged: `oglu_load_dll` has no caller → opengl32.dll is never in the process → `getgl` resolves nothing → `tagpu_gaf_atlas_mirror_rgb` returns at its entry guard before its own `calloc` |

Any **one** of those makes the removed code unreachable. That is worth stating precisely because
it is the difference between this landing and a feature removal, and part 1's first draft got it
wrong in the other direction — it called the GUI's mirror "a feature" on the strength of the
third pin alone, when the first two had already made the whole colour route dead two landings
earlier.

#### WHAT WENT

| file | what left |
|---|---|
| `tagpu_vk_gui.c` | the `s_ar*` RGBA8 image (`s_arImg`/`s_arMem`/`s_arView`/`s_arDim`/`s_arRows`/`s_arHave`/`s_arNeedClear`/`s_arSerial`), its creation clear, its shrink clear, its share of the staging reservation, both `if (arUp)` blocks — staging fill, and barrier/clear/copy — and the `atlasRgb` bounds check. Binding 41 now takes `VK_NULL_HANDLE` where it took `on ? s_arView : VK_NULL_HANDLE` |
| `tagpu_gui.h` | `atlasRgb`, `atlasRgbRows`, `atlasRgbSerial` out of the thread hand-over |
| `tagpu_gui_surf.c` | the arm/step pair (`tagpu_gaf_atlas_mirror_rgb` at `:2123`) and the publication block that begins at `:2322` with its else branch — line numbers as `main` has them |
| `tagpu_gaf.c` | `tagpu_gaf_atlas_mirror_rgb`, `_step`, `rgb_mirror_zeroed`, `getgl`, `fetch_gl`, `x_glReadPixels`, the `PFN_READPIXELS` typedef, the three `mirrorRgb*` clear sites, and `#include "opengl_utils.h"` |
| `tagpu_gaf.h` | every `mirrorRgb*` field and both function declarations. `rgbAniso` stays — it is the other lane's sampler ratio, `[PINNED 0.0f]`, and unpinning it is 11-5e-2c's |

Two refusals lost a term rather than a branch, which is the shape to check twice: `if
(h.colourTwins && !h.atlasRgb)` became `if (h.colourTwins)`, and `(o->col & TAGPU_GUICOL_ON) &&
!h.atlasRgb` became `(o->col & TAGPU_GUICOL_ON)`. Both are type-identical to what they evaluated
to, because the dropped term was constant-true; the first pin says the surviving term is
constant-false anyway.

#### THE COUNTS, comment- and string-masked as before

| pattern | main (`5d34cdb`) | after | where the remainder is |
|---|---|---|---|
| `mirrorRgb`-or-`mirror_rgb`, substring | **57** — `tagpu_gaf.c` 40, `tagpu_gaf.h` 10, `tagpu_gui_surf.c` 7 | **0** | nothing. The spelling leaves the tree |
| `atlasRgb`, substring **and** `\b`-anchored | **30** — `tagpu_vk_gui.c` 16, `tagpu_gui_surf.c` 6, `tagpu_gui.h` 3, `tagpu_posedraw.c` 2, `tagpu_vk_unit.c` 2, `tagpu_posedraw.h` 1 | **5** | all five are `atlasRgbAniso`: `tagpu_posedraw.h:425`, `tagpu_posedraw.c:667` and `:678`, `tagpu_vk_unit.c:1965` and `:1971` |
| `tools/gl-sites.py` narrow / wide | 252 / **323** | 252 / **322** | the four files of escalation reason 1 |

The `atlasRgb` row starts at 30 where §2.76 reported **29**, and the extra site is one §2.76's
own review put there: the fix for its latent-clarity finding added `s_pub.atlasRgbAniso = 0.0f;`
to `tagpu_posedraw.c`'s else branch, so the count moved between the docs commit (`1c4dced`, 29)
and the review commit (`6a00b8d`, 30). Both figures are right about their own tree; this one is
right about `main`. Counted at every tip of the landing so the arithmetic can be checked:
`67271d9` 112, `1f329d0` 29, `1c4dced` 29, `6a00b8d` 30, `5d34cdb` 30, `c76b56d` 5.

#### `tagpu_gaf.c` IS NOW GL-FREE ON BOTH COUNTS, WHICH IS A DIFFERENT CLAIM FROM ZERO CALLS

The plan has said since 11-5e-2 that this file is where "makes no GL call" and "free of GL" come
apart: its narrow count was already 0, and its wide count was 1 — the `xwglGetProcAddress` inside
`getgl`, resolving `glReadPixels` through WGL. That one is gone with the mirror, so the file now
names **no GL type, constant or entry point**, and `opengl_utils.h` goes with it: includers
**6 → 5**, and every survivor has a stated reason —

| includer | why it still includes it |
|---|---|
| `opengl_utils.c` | it defines the entry points |
| `render_gdi.c` | it reads `g_oglu_version` |
| `tagpu_hires.c` (30 sites), `tagpu_hires_draw.c` (104), `tagpu_shadow.c` (87) | **escalation reason 1** — these three plus `opengl_utils.c` are why 11-5e cannot finish on its own |

**`tagpu_restoreglsl.h` is NOT one of them and the include stayed.** It reads as a GL header from
its name and it is not: what `tagpu_gaf.c` takes from it is `TAGPU_RGLSL_FRAME` and
`tagpu_rglsl_tileable`, both still live, neither GL. A deletion pass that reads names would have
taken it — which is part 1's near miss (§2.76) in its other form, a name that suggests a
dependency the file does not have, rather than a name that hides one it does.

#### AND THE UI NOW HAS NO ROUTE TO CLASSIC++ COLOUR AT ALL — a named gap, not a silent one

Every world atlas that lost its read-back had somewhere to go: `tagpu_gaf_atlas_restore_vk` is
armed for the unit, feature and effects atlases, and the other lane paints their twins on the
device from the published list. **`tagpu_gui_surf.c` never arms it.** So after this landing the
UI atlas has no read-back and no list, and restored art cannot reach a UI sprite by any route in
the tree.

Nothing regresses today, because the first two pins above already made that art unreachable back
at 11-4b. But the plan said *"the UI atlas is a fifth consumer and keeps its read-back"*, and
after this landing that sentence has no referent — the fifth consumer has no feed at all. Giving
the UI colour again means **arming the list for it and consuming it**, which is new work of the
same shape as 11-5e-2c rather than anything a deletion pass can leave behind. Stated here so the
next reader finds the gap written down instead of inferring it from an empty branch.

#### LEFT STANDING DELIBERATELY

The UI's colour-twin **subsystem** — `colourTwins`, the per-twin `colImg`/`colView`/`fb2`, the
`TAGPU_GUICOL_*` bits, the second render pass and the four `*2` pipelines — is untouched. It is
dead by the first two pins, it holds **no GL**, and it is a different question from the read-back:
this gate is about GL, and a subsystem that answers to the colour decision above should be
removed by whoever makes that decision. Filed as its own item rather than swept in here.

#### MEASURED — four runs, INTERLEAVED, and the interleaving is the whole design

`e5a`, scenario `one-unit`, 1024x768, `restorevk.on` + `classicpp.on`, `--maxfps 0`, two DLLs
built once and never rebuilt during the run: `main` at 1 558 016 bytes, this branch at
**1 547 776** (10 240 smaller). Order **main, branch, main, branch** — alternating, not
three-and-three.

| pair | kind | whole frame | outside the minimap |
|---|---|---|---|
| grabs 1/2/3 within each of the four runs (12 pairs) | static check | **0 px** | 0 px |
| `mainA` v `brA` — adjacent in time | **cross-build** | **0 px** | 0 px |
| `mainB` v `brB` — adjacent in time | **cross-build** | **0 px** | 0 px |
| `mainA` v `mainB` — same build, across the flip | **control** | 44 px | **0 px** |
| `brA` v `brB` — same build, across the flip | **control** | 44 px | **0 px** |
| `mainA` v `brB`, `brA` v `mainB` — across the flip | cross-build | 44 px | **0 px** |

Both adjacent-in-time cross-build pairs are **byte-identical PNGs**. The fixture changed mode
between the first pair and the second — the same bimodality §2.76 found, one off-camera unit's
minimap dot — and **both builds followed the fixture rather than each other**: the same-build
control across the flip differs by exactly the same 44 px as the cross-build pairs that span it.
So the build accounts for **0 pixels**, and the 44 are the fixture's, entirely inside the
minimap rect.

**That is what interleaving buys, and it is worth stating as the method rather than the result.**
§2.76's measurement was batched three-and-three; it read as a clean build difference and took a
fifth run of `main`'s own DLL to expose as drift. Alternating puts **both builds in both modes by
construction**, so four runs settle what nine could not — the control is not a separate
experiment run afterwards, it is the same experiment read down the other diagonal.

**The log is the sharper oracle here, because part 2's one new risk has one.** Binding 41 now
takes `VK_NULL_HANDLE` unconditionally, and a null descriptor the shader still sampled would be a
validation error or a device loss — loud in the log, nearly invisible in a screenshot. Across all
four runs: **zero** `VK_ERROR`, `DEVICE_LOST`, `VUID` or validation-layer lines, and every
`viol`/`pviol` counter 0. The restore lines come out as an **identical multiset in all four**,
16 lines each, including the ones that say the live route ran —

```
unit: restorevk -- ... no read-back and the frame list is published instead (25 entries seeded, 8192-frame bound)
vk: unit: restoring the twin HERE - 25 of 25 frames over 2048x2048, generation 1
vk: terr: restoring the tile atlas HERE - 5062 frames over 4096x4096
vk: unit: restored twin painted here - ... no mirror and no read-back
```

The fork's own log has been saying *"no mirror and no read-back"* since the list path landed.
This landing is the point at which that sentence stops being a description of one lane's choice
and becomes a property of the tree.

#### AND THEN THE MEASUREMENT ABOVE TURNED OUT NOT TO COVER THIS LANDING

The four runs above had **`tagpu_gui.on` OFF**, which their own logs say in terms
(`gui: trigger host only (tagpu_gui.on is not on) — the UI layer, the census and the 17 leaves
are OFF`). They exercised the world passes and proved this landing broke none of them, which is
worth having. They did **not** run the GUI lane, and the GUI lane is this landing's whole
subject. Found by chasing a review finding that did not reproduce (below), not by re-reading the
plan — the label said "part 2 measured" and the runs had never entered the code.

**Re-run, same interleaving, `gui.on=1`.** The layer comes up armed — `gui: ARMED
flip@0x4C63A0=1 leaves=17/17`, `twins=3`, and between **577 311 and 600 695 `twin_sprite` calls**
and **4 355–4 517 `twin_copy` calls** per run. Twelve grabs fall into **four** distinct frames,
which decompose as **two independent oscillators**, neither of them the build:

| oscillator | size | where | seen on |
|---|---|---|---|
| the cursor's own pixel | **1 px** | exactly (512, 384) — the hotspot; `curs=1,10x20,drawn=293` | both builds, both states |
| the minimap cluster | **48 px** | x[63..104] y[47..125], inside the minimap rect | the same bimodality §2.76 found |

**Mask those two regions and all twelve grabs are byte-identical** — one hash across four runs
and both builds, over 733 323 non-black pixels of compared frame. The entire UI layer — panel,
bars, dialogs, 3 twins, 28 atlas frames, half a million sprite ops — is pixel-identical.

Honest limit: in **this** experiment the minimap's second mode appears only in `brB`, so these
four runs do not by themselves show that cluster is build-independent. What shows it is the
`gui.off` experiment above, where `mainA` and `mainB` differ by exactly that cluster and nothing
else, and §2.76's five runs of `main`'s own DLL producing both modes.

#### AND "NOTHING OBSERVABLE CHANGES" IS OVERSTATED BY EXACTLY ONE LOG LINE

`c76b56d`'s message opens "WHAT IT CHANGES: NOTHING OBSERVABLE". With the UI layer on, that is
wrong by one line, and the review predicted it before the run did:

```
gui: no glReadPixels/FBO entry points - the restored twin cannot be mirrored and
     the Vulkan edition stays indexed
```

`main` writes it **once per process** — `tagpu_gui_present` calls the arm every present,
`s_mirWant` is 1 because `tagpu_vk_gui_prepare` asks for the mirror, and the refusal latches on
`mirrorRgbFailed` so it is said once. Measured: **1 on each `main` run, 0 on each branch run**,
and the string is absent from the branch's `ddraw.dll` entirely. It is the only line the landing
removes, and losing it is an improvement — the plan already called it "a line that reads as a
driver fault on a perfectly normal run" — but anyone log-diffing this branch will find it, so it
is named here rather than discovered.

Everything else on the log side is clean: **zero** `VK_ERROR`, `DEVICE_LOST`, `VUID` or
validation-layer lines on any of the four runs, which is the check binding 41's unconditional
`VK_NULL_HANDLE` needed.

#### THE PINS, MEASURED RATHER THAN ARGUED

Until this run the three pins were a reading of the source. The GUI's own counter dump states
two of them as numbers, identically on **both** builds and all four runs:

```
twins=3 ... col=0/3 colvalid=0 rearms=0 rgb=0
```

`colvalid=0` is pin 1 — `s_colValid` never left its initialiser. `col=0/3` is pin 2 — zero of
three surfaces got a colour twin, across **over half a million** `twin_sprite` calls and some
4 400 `twin_copy` calls in a single session. The removed code was not merely unreachable on
paper; the engine ran the two functions that would have armed it 580 000 times and they returned
0 every time.

#### GATES

`make -C tagpu/ddraw -j$(nproc)` clean; `thread-split: clean — 34 listed file(s)`; `spirv: 49
shaders, 33 programs, headers current`. The function inventory against `main` shows exactly the
six intended removals in `tagpu_gaf.c` and **no function added or removed** in `tagpu_vk_gui.c`
or `tagpu_gui_surf.c` — the check that a scripted cut did not swallow a neighbour, which is a
trap this gate has paid for before.

### 2.79 The restore feed, put back between a bound and an ordering — landing 11-5e-2c

The published restore list is the other lane's only input, and it has been
**seeded once per session and never fed since landing 11-4c**. `atlas_paint` ended
in `if (a->job) restore_enqueue(a, e);` — the GL restorer's job — and `a->job` went
NULL when 11-4c took the `glGenTextures` out of `tagpu_gaf_atlas_create`. A predicate
that read as *"the GL restorer is running"* had quietly become *"the GL restorer can
never run"*, and it was gating the **other backend's** feed. 11-5e-2 found the one-line
fix, proved it was a use-after-free, and took it back out. This is that fix with the
two guarantees it needed.

#### THE HAZARD WAS NEVER A RACE, AND THE PROSE THAT SAID SO INVITED THE WRONG FIX

`tagpu_gaf.c` described *"the buffer the render thread is about to read"*, and the plan
agreed. That phrasing asks for a lock or a fence, and **a fence here would have fixed
nothing while reading as though it had**. Established by call graph instead:

| step | where | thread |
|---|---|---|
| `render_vk.c:232` | `tagpu_overlay_draw` — the whole gather and paint half | render |
| ↳ `tagpu_overlay.c:264` | `tagpu_native_frame` (its only caller) | render |
| ↳ `:3291`, and `ghost_record` → `ghost_one:1887` | `tagpu_posebake_unit` → `mat_bake` → `pb_walk` → `mat_emit:467` → `tagpu_r3d_atlas_uv` → `atlas_paint` | render |
| ↳ `tagpu_native.c`, `tagpu_fx_gather` | `emit_model` → `tagpu_fxmodel_raster` → `tagpu_r3d_atlas_texels` → `atlas_get` → `atlas_paint` | render |
| `render_vk.c:268` | `tagpu_vk_frame` → `vk_present` → `tagpu_vk_unit.c:1803` → `tagpu_posedraw_handover` | render |

Every paint of an armed atlas and every consumer are the **same thread**, in one
iteration of one loop. The unit atlas is reachable only through `tagpu_posebake.c`'s
`mat_emit` and `tagpu_fxmodel.c`'s `tagpu_fxmodel_raster`, both inside `tagpu_native_frame`, which has exactly one
caller, which has exactly one. The GUI atlas is **not in scope**, and the reason is that
it never arms a list — `tagpu_gaf_atlas_restore_vk` has exactly three callers
(`tagpu_fx.c:173`, `tagpu_feat.c:267`, `tagpu_render3do.c:555`) and none is in
`tagpu_gui_surf.c`. It is *not* because it is painted from the game thread: the review
disproved that, and both of its paint sites are on the render thread too
(`tagpu_gui_surf.c:1233` via `drain`, `:1452` via `sharp_begin`, both inside
`tagpu_gui_present`, called from `tagpu_overlay.c:270`). The game thread only queues ops. So this is an **intra-thread ordering**
defect: a pointer captured early in `overlay_draw`, a paint later in the same call,
and a read later still.

#### THE BOUND — the address cannot move

`tagpu_gaf_atlas_restore_vk` allocates `rlist_cap(a)` frames in **one `malloc`**
instead of 256-and-grow, and `rlist_room` becomes a pure comparison. There is **no
`realloc` anywhere in the file**, so the address never moves while the atlas holds it.
The cost is stated rather than hidden:

| atlas | `ATLAS_MAX` | `rlist_cap` (`max × 4`) | bound at 44 B/frame |
|---|---|---|---|
| unit (`tagpu_render3do.c`) | 2048 | 8192 | **360 448 B** |
| effects (`tagpu_fx.c`) | 2048 | 8192 | **360 448 B** |
| feature (`tagpu_feat.c`) | 4096 | 16384 | **720 896 B** |

Against the 16 MB RGBA read-back mirror this same file declined to free in the ordinary
case — that buffer is gone entirely since 11-5e-2b, and the 8-bit `mirror` the file still
owns is `dim × dim` = 4 MB.

**AND THE ONE `free`, which the review caught this section denying.** The first draft said
`a->rlist` had "exactly one writer in the file and no `realloc` and no `free` anywhere in
it" — and this same landing added `free(a->rlist)` to `tagpu_gaf_atlas_free_buffers`
(`tagpu_gaf.c:638`), which the section itself describes forty lines further down. Three
comments in the shipped code repeated the same false absolute; only one worded it
correctly. What is true is narrower:

- there is no `realloc`, so the address never moves while the atlas holds it;
- the single `free` is the atlas's own teardown, and it clears `rlist`, `rlistWant`,
  `rlistFailed` and both counts together, so no later append can reach a stale pointer.

**What it does not bound is a consumer that captured the pointer before that `free` ran**,
and that is unreachable today only because `tagpu_gaf_atlas_free_buffers` has exactly one
call site — `tagpu_gui_surf.c:643`, the GUI atlas, which never arms a list. That is an
argument about the *caller*, which is the shape this landing set out to replace. It is
written down rather than left to be re-derived, in the note and at `rlist_add`'s own
header: **a second caller, on an armed atlas, re-opens the hazard.**

#### THE ORDERING — the in-place rewrite cannot reach a reader

The bound does not cover `rlist_restart`, which zeroes the count and refills the array
**in place**. So the unit list moves from `pd_view_publish` to
`tagpu_posedraw_handover`. The old site is the *first* posedraw window of the frame,
with `ghost_record` still to run and still able to paint through
`tagpu_r3d_atlas_uv` → `atlas_get` on a build ghost not yet atlased. The new site sits
inside `tagpu_vk_frame`, which the loop reaches only after `tagpu_overlay_draw` has
returned — so every paint of the frame is behind the capture **by the loop's shape**.

It joins `s_pub.otherDraws`, which is taken at that same beat for the same reason, and
whose comment had said `render_ogl.c` since before the Vulkan lane existed. Corrected:
the property is the loop's shape and both loops have it, but the file named was the one
that no longer runs.

**Two hazards, two different guarantees, and neither is a timing argument.**

**The indexed mirror's serial, rows and pointer belong on the same beat, and until
2026-09-23 they were not.** They stayed in `pd_view_publish` when the list moved, so a
build ghost that atlased a texture for the first time published the new frame alongside
the *previous* `mirrorSerial`. `tagpu_vk_unit.c`'s `atlas_upload` saw a serial it already
held and copied nothing, `covered_prefix` passed the frame (same shelf, rows already
covered), and the restorer painted the cell from the device's stale texels: palette
index 0, opaque black, for the rest of the generation. Seen on the first ARMMEX placed
through the ghost: its plate was black on every extractor built after it, under
`assets=1` only. Both reads are now in `tagpu_posedraw_handover`. `atlas_paint`
(`tagpu_gaf.c`) writes the bytes, bumps the serial and appends to the list in one call, so
a serial and a list read together agree by construction. Repro:
`scenarios/mex-ghost.json`: select the commander, place an ARMMEX, and wait for it to
finish.

#### OVERFLOW RESTARTS RATHER THAN DROPS

`rlist_restart`'s own definition already called itself *"both the arm path and the
overflow recovery"*, so the append uses it. The bound is `max × 4`, so reaching it means
a repack storm re-laid the atlas three times over; the restart reseeds from what the
atlas actually holds, cannot itself overflow (at most `max` entries against `max × 4`),
and says so in the log. The bounds test is re-checked afterwards anyway, because
*"cannot happen"* is not an argument for writing past an array.

#### THE TWO PINS, AND THE SECOND IS NOT WHAT THE PLAN EXPECTED

**`restored` asked a GL question.** All three producers gated it on the atlas's GL
texture name — `tagpu_posedraw.c` through `tagpu_r3d_atlas_rgbref()`, `tagpu_feat.c`
and `tagpu_fx.c` on `s_atlas.rgb` — and that name has exactly **one writer in the tree**,
`tagpu_gaf.c:661 a->rgb = 0`. It published 0 on every frame since 11-5e-2 deleted the
restorer. It now asks whether the published list is armed, which is the route that
exists. In `tagpu_posedraw.c` the assignment also **moved below the arm it asks about**;
it had sat 57 lines above `tagpu_r3d_atlas_restore_want()` (`main`'s
`tagpu_posedraw.c:601` and `:658`), harmless only while it read a pinned 0.

**`rgbAniso` carries the knob, not the documented constant, and that reverses a
review's instruction.** 11-5e-2's review left a warning at the field calling it a loaded
gun: the consumer stands the whole frame down on `h.atlasRgbAniso != s_twinAniso`, so a
field reporting 0 against a sampler built at 4 draws no unit at all. Right about the
danger — and one detail of it was wrong, and the detail is the whole decision.

> **`s_twinAniso` is not `aniso=`. It is `aniso=` CLAMPED BY THE DEVICE**, and
> `tagpu_vk_unit.c` sets it to `0.0f` unless the extension is present *and*
> `d->maxAniso >= want`.

So publishing `TAGPU_GAF_TWIN_ANISO` — the fix the warning asked for — would have
swapped one silent stand-down for another, on every device without anisotropic
filtering, over a feature it can do nothing about. The guard has a **third** term the
first draft of this section left out, and it is the one that bites soonest:
`want > 1.0f` (`tagpu_vk_unit.c:863`). `aniso=1` is the documented A/B setting, and at
`aniso=1` the applied value is `0.0f` on *every* device, not only on one without the
extension. That is the plan's own predicted
failure arriving from the other side.

The producer publishes `tagpu_classicpp_light()->aniso` on every arm beat — **every
beat, not at the arm**, because the case worth catching is a knob edited *after* the
latch. The consumer keeps `s_twinAnisoWant`, the value it read, separately from
`s_twinAniso`, the value the device allowed, and compares against the former. The test
then means *"the two ends are configured apart"* — exactly what the sampler's own
comment says it is for, a knob changed mid-session when the sampler cannot be rebuilt
because every other slot's submit still names it — and stops meaning *"the device
lacks a feature"*. Publishing what the consumer actually applies was the third option
and is worse than both: the test would compare a value against itself.

Both prescriptions in `tagpu_gaf.h` are corrected rather than left standing — the
loaded-gun comment now says why the fix is not the one it asked for, and
`TAGPU_GAF_TWIN_ANISO` records that the landing which brought the feed back decided
against carrying it.

#### AND THE LAST LANDING'S F4 IS NOW WHOLE

`tagpu_gaf_atlas_free_buffers` frees `rlist` as well as `mirror`, and clears
`rlistWant`/`rlistFailed` with it — an atlas with the latch set over a NULL buffer is
precisely the half-state that review named. 11-5e-2b part 2 declined the fix on purpose,
because a half-fix was worse than the gap and `rlist`'s lifetime belonged here. **The
bound is what made it a complete statement**: one allocation site, no `realloc`, and this
the only free, so "give it back and clear the state that described it" is the whole of the
lifetime rather than one end of one nobody owned — with the caller-side residual named
under the bound above. The header's declaration comment is corrected in the
same commit — the `.h` half of a `.c` fix is the miss the last review caught twice.

#### WHAT IT CHANGES ON SCREEN — and it is not nothing

The two pins above are described as flags, which makes them sound like bookkeeping.
`restored` is not bookkeeping: it reaches the **fragment shader**.

| step | file:line |
|---|---|
| producer publishes the flag | `tagpu_posedraw.c` (below the arm, since this landing) |
| consumer copies it into the uniform block | `tagpu_vk_unit.c:1710` — `b.i[0] = h->restored;` |
| shader declares it | `tagpu_native.c:442` — `uniform int uRestored;` |
| shader acts on it | `tagpu_native.c:470` — `if (uRestored == 1) t = texture(uAtlasRGB, vUV);` |

With the flag at 0, `t` stays `vec4(0.0)` and the fragment falls through to the
palette-indexed path. So the restored twin was **built, painted, mipped and bound** —
`tagpu_vk_unit.c:2633-2634` binds `s_arView`/`s_sampTwin` at slot 3 on the strength of
the view existing, not of the flag — and then **never read**. Every session since
11-5e-2 has paid for a restore whose output no shader sampled.

#### WHAT THE REVIEW CHANGED, BECAUSE IT IS THE LARGER HALF OF THIS LANDING

Two independent reviewers found the same defect and one filed it HIGH: **unpinning `restored`
made a stand-down reachable that had been dead code, and it does not stand down for a frame, it
stands down for the session.** `atlas_rgb_build`'s own header had already written the contract
the unpin broke —

> *"A FAILURE HERE IS NOT FATAL AND MUST NOT BE. The view stays NULL, binding 43 falls back to
> the indexed view exactly as it did before gate 3 … so a device that will not give us 16 MB of
> RGBA8 loses restored frames rather than the pass."*

— and that guarantee rested entirely on the flag being 0.

| case | mechanism | effect |
|---|---|---|
| **failure** | `s_rjTried` latches at five sites in `restore_want`, cleared only by teardown; its own first guard `if (s_rjTried) { s_arHave = 0; return; }` (`tagpu_vk_unit.c:1301`) then forces it | one Vulkan resource refusal ⇒ **no unit drawn at all for the session** |
| **routine** | `rlistRepaint` has one assignment and **both** call sites pass 0, so `if (!repaint) s_arHave = 0;` fires on every generation change | every level boundary, atlas recycle and map change blanks **all** units, features and effects for the length of a repaint |

The routine case is the worse one because nothing has to fail for it.

**The fix is this landing's own argument carried through, not a new one.** The stand-down existed
to avoid showing *"a different picture from its own oracle"* — art filtered unlike the other
lane's. There is no other lane, and `tagpu_gaf.h` says so in this same landing. Indexed art is
not a disagreement with anybody; it is Classic++ restore off for one frame. So a missing twin
clears `h.restored` — `uRestored` goes 0 and the shader takes the palette path — in all three
passes and in the aniso gate, whose penalty was likewise the whole pass for an edited cfg file.
**A bound on what the flag may promise, not a timing mitigation.**

**AND THE FIRST CUT OF IT ONLY COVERED THE RARE HALF, WHICH THIS SECTION CLAIMED OTHERWISE.**
It cleared the flag on `!feed` alone. A generation change satisfies every feed term —
`restoreFrames` non-NULL, `s_arImg` built, `s_rjTried` still 0 — so it took `feed = 1` and left
through `feedout` drawing nothing, which is the routine case the table above calls the worse
one. A verification pass found it, and found this paragraph asserting it was fixed. The flag is
now cleared on **both** branches and the feed frame draws indexed; `restore_want` takes no
command buffer and never reads `restored`, so the job is still made and nothing it recorded can
conflict with a draw.

**The descriptor had to follow, and this is the part the fix could not have been complete
without.** Binding 43 named the twin on `s_arView` alone. `mk_image` creates that image
`UNDEFINED` and only the restorer's job transitions it, so a frame drawing with the image made
but unpainted declares `SHADER_READ_ONLY_OPTIMAL` over an `UNDEFINED` image — survivable only
while the removed stand-down kept those frames from drawing at all. It now tests
`s_arView && s_arHave`, which is exactly *"this image has left UNDEFINED"*, and is the same pair
that decides `uRestored`: flag and descriptor agree by construction rather than by a refusal
placed somewhere else. `tagpu_vk_terr.c:1080` settled this for binding 42 in the 11-5c
re-review and carries the argument; this is that shape, adopted rather than reinvented.

**Two regressions the first cut introduced, both found by the same pass.** It killed the
`s_saidAniso` latch — `goto standdown` had made the reset unreachable while the condition held,
so a fall-through runs it on the pass that sets it and the line logs *every frame* — and it left
the unit pass's two messages saying "nothing drawn" and "the other lane" on paths that now draw
and about a lane that does not exist. **A third said the same thing and was missed until the
citation pass:** the aniso gate's own message ended *"nothing is drawn on a frame that samples
it"* while the two lines below it now clear `h.restored` and draw indexed. It says *"this frame
draws the indexed atlas"* now. All three fixed; the re-measurement's paired logs are 6284
lines for `main` A and 6284 for branch A, 6275 and 6276 for the B pair, so nothing leaked into
per-frame logging.

**Four log lines and two comments still say "the other lane", and this landing did not reach
them.** It reworded the messages of the three passes whose logic it rewrote; still printing on
builds that have no GL lane at all are `tagpu_gaf.c:591`, `tagpu_terr.c:261`,
`tagpu_vk_gui.c:1583` and `tagpu_vk_restore.c:2127` ("the same integer (sum+1)/4 **the GL lane
draws**"), with the same phrase surviving in comments at `tagpu_vk_feat.c:1059` and
`tagpu_vk_fx.c:1095`. All are 11-5e-2 vintage. Strings and comments are not behaviour, so they
cost a wrong sentence in a log rather than a wrong frame, and the sweep belongs to the deletion
landing, where "there is no other lane" stops being a claim in prose and becomes the tree.

Two more sit in this file and were deliberately left: `tagpu_vk_unit.c:1862` (*"the GL twin's
shadow PCF is bilinear"*) and `:2674` (*"the GL twin drew these units against a cast-shadow
map"*). Unlike the three above, **their "nothing drawn" is still true** — both really do stand
down — so only the *reason* they give is stale, and they name the shadow path, which is the
deletion landing's subject rather than this one's.

**One of the four is not only a string, and it is named here so the next landing does not have
to find it twice.** `tagpu_vk_gui.c:1583` guards a stand-down of exactly the shape this section
just removed from three passes — *"the other lane is compositing Classic++ colour and this one
has no restored atlas to composite from - nothing composited while that is true"* — on the GUI
pass, which is outside this landing's diff and was therefore not verified, measured or changed
here. Whether its `h.colourTwins` gate is reachable on a Vulkan-only build is an open question,
not a finding.

Three more the reviews caught, each verified against the source before it was acted on:
`tagpu_posedraw.c` asked `tagpu_classicpp_on()` where feat, fx and terr ask
`tagpu_classicpp_assets()`, so turning assets off mid-session left units on the twin while
everything else reverted — mixed art, silently, until restart; the hand-over now clears
`restored` alongside the list it promises, making "flag set, list absent" unrepresentable rather
than merely unreached; and `tagpu_gui_surf.c:643`, the **one** caller of
`tagpu_gaf_atlas_free_buffers`, still said that call does not free `rlist`.

#### THE MEASUREMENT — TWO FIXTURES, BECAUSE NEITHER CAN DO BOTH HALVES

**The first attempt proved nothing about the feed, and the review is what established that.**
Both builds showed identical restore histories — `25 of 25 frames`, one generation, the bound
never restarted. Equivalent histories mean **no frame was ever appended**, so the pixel
difference was attributable to the `restored` unpin alone and the bound, the ordering and the
append — the landing's title — had no positive evidence at all. `one-unit` places one unit; its
textures are all in the atlas before the arm, so the append has nothing to do.

**Fixture A — `crowd-static`, for the counters.** 256 units of **16 types**, one owner, no
orders. The types atlas their textures long after the arm, which is what makes a feed visible.
Clean-built DLLs, interleaved, two runs each:

| run | arm seeds | restore starts | **twin painted** |
|---|---|---|---|
| `main` A | 25 | 25 of 25, generation 1 | **25 frames** |
| branch A | 25 | 25 of 25, generation 1 | **158 frames** |
| `main` B | 25 | 25 of 25, generation 1 | **25 frames** |
| branch B | 25 | 25 of 25, generation 1 | **158 frames** |

`main` paints exactly what the arm seeded and never another frame, because on `main` there is no
append at all — `restore_frame_of`'s only caller is `rlist_restart`'s sweep of `a->ents`. The
branch appends **133 more**, identically in two independent runs. Zero VK errors, zero bound
restarts, on both. **That is the feed, measured.** Re-run unchanged after the fix above, with
the branch-only line `a Classic++ restore is armed and this lane's twin is not painted yet —
drawing the indexed atlas until it is (this frame also makes the restore job)` appearing exactly
once: the frame that now draws is precisely the feed frame that used to return blank.

**Fixture B — `one-unit`, for the pixels**, because `crowd-static` fails gate 1 outright: its
units run their idle COB scripts and three grabs 4 s apart differ by ~12 000 px on every build.

| | total | minimap | unit box | elsewhere |
|---|---|---|---|---|
| gate 1, settled pair, worst of four runs | 1 | 0 | 1 (the cursor) | 0 |
| **control** — `main` A vs `main` B | 48 | **48** | 0 | **0** |
| **control** — branch A vs branch B | 0 | 0 | 0 | 0 |
| cross-build, **all four** pairings | 302 | 44 | **258** | **0** |

**258 px of 786 432, in the box of the one placed unit** (`world=(1600,1600,91)
screen=(512,384)`), identical across all four cross-build pairings, and **0 px everywhere
outside the minimap and that box**. The box is not asserted, it is **derived**: the union of
the four cross-build bounding boxes, `x 502..516 y 355..388`, 15×34 px, centred on the placed
unit — so it cannot have been fitted to the answer. The census is 749 452 non-black px on every
one of the four runs, so no arm drew an empty frame. Zero VK errors on all four. The figure is
unchanged by the H1 fix, which is the expected result: the fix only alters what happens when the
twin is *absent*, and at steady state both builds have one.

**It was run twice, and the second run is the one that ships.** The first ran against a branch
DLL that predated one `plog` format-string correction (the aniso gate still said *"nothing is
drawn"*); it gave 258–259 px in the same derived box and 0 px outside it. The re-run above is
built from the committed tree. A format string reaches the log file and nothing else, so the
agreement is expected rather than informative — but the number that is written down is the one
measured on the tree that lands.

#### THE FIXTURE LESSON, WHICH COST THIS LANDING TWO ROUNDS

**`one-unit` is a live skirmish.** It loads with three AI opponents — 15 to 17 units alive,
commanders walking, `corsolar` and `corwin` under construction. Only the *placed* unit is still.
The 44 px inside the minimap are our own commander's blip, and the control now shows 48 px of
the same drift **between two runs of `main` itself**, so excluding that region is supported by
measurement and not only by the construction argument (a renderer cannot move a unit).

The first write-up had to exclude it by construction alone, because two runs per build cannot
separate "the build did it" from "a wandering commander split along the build" — and a 0 px
control does not help, since a commander with two resting places produces exactly that. **This
run's control is the honest version of that claim.**

**AND ONE GRAB IN TWELVE WAS AN OUTLIER, WHICH IS WHY THE METHOD TAKES THREE.** In the first
run's `main` A arm, grab 2 differed from grabs 1 and 3 by **40 556 px** — scattered over
x[220..1023] y[392..735], 99 of the 768 32×32 tiles, mean |Δ| 126 — while grabs 1 and 3 agreed
**0 px inside that whole region**. The other eleven grabs across the four arms were 0–1 px
apart, and no other arm showed it: one capture caught a transient the live skirmish produced,
not drift. With two grabs the run reads as a fixture that is not static and the measurement is
discarded, or the outlier becomes the cross-build comparand and invents a 40 000-px "build
difference". **Gate 1 is read on the settled pair; two of three disagreeing is the failure it
is actually looking for.** [Added to the `ta-drive` skill.]

**`crowd-static` is static as a situation, not as pixels**, and its own description says the
first thing without the second. It is the right fixture for a counter at scale and the wrong one
for a frame.

**So pick the fixture per oracle, not per landing.** Wanting one fixture to do both halves is how
the first attempt ended up with a measurement that never touched the code the landing is named
for. The four gates bound drift between and within runs; none of them bounds a second player, and
none of them notices that the thing under test was never exercised.

### 2.80 Two features that were already dark, and the deletion that made them visible — landing 11 D1–D4

**2026-09-23: the rest is gone too.** The Vulkan caster pass `tagpu_vk_hires.c` (with `tagpu_vk_hires.h`, `src/tagpu_hires_glsl.h`, `inc/tagpu_hires.h`, the `hires` SPIR-V program and the `scenarios/hires-*.json` fixtures) and the unused `native_c` composite program (`CVS`/`CFS` in `tagpu_native.c`) are deleted: nothing produced the first's hand-over and nothing drew the second. The shadow pass itself stays.

The gate's remaining work was filed as "delete four GL files". Surveying it after Decision 1
found that **three of the four are not what the row assumed**, and one of the findings is not
about deletion at all: cast shadows and glTF replacement models are **off on the shipped lane
today**, each for its own reason, and the two halves of each went dark together so that nothing
stood down and nothing logged.

Everything below was checked with comments and string literals masked (`count_masked.py`). That
is not fastidiousness: a plain grep is wrong in both directions here and it caught this session
out once. The first pass of the survey reported four surviving files as including
`opengl_utils.h`; the hits were prose *about* its removal. **The real directive count is five —
`opengl_utils.c`, `tagpu_shadow.c`, `tagpu_hires.c`, `tagpu_hires_draw.c` and `render_gdi.c` —
and four of the five are the doomed files.**

#### THE SHADOW CHAIN, EVERY LINK

| fact | where | consequence |
|---|---|---|
| `s_live = 1` occurs **once** | `tagpu_shadow.c:378`, inside `tagpu_shadow_begin` | which is **callerless** |
| `s_pubHave = 1` occurs **once** | `tagpu_shadow.c:506`, inside `tagpu_shadow_end` | which is **callerless** |
| ⇒ `tagpu_shadow_live()` | returns 0 for the life of the process | `shadowOn` is 0 at both producers (`tagpu_posedraw.c`, `tagpu_terr.c`) |
| ⇒ `tagpu_shadow_handover()` | fails its first term every frame | `tagpu_vk_shadow_prepare` returns 0, `s_liveHave` never set |
| ⇒ `tagpu_vk_shadow_ready()` | answers false, always | and **nothing stands down**, because the two consumer refusals (`tagpu_vk_unit.c:2670`, `tagpu_vk_terr.c:1407`) are themselves gated on `shadowOn` |

With comments and strings masked, eight of the thirteen `tagpu_shadow_*` entry points appeared
nowhere outside `tagpu_shadow.c` and `tagpu_shadow.h`.

**The two `tagpu_shadow.c` line numbers in that table are into the file D2 deleted**, at its last
revision — `db6fe8d^`, which is `ee47584`. Every other citation on this page is live. They are
kept as numbers rather than softened to names because reconstructing the argument means reading
those two assignments, and `git show db6fe8d^:tagpu/ddraw/src/tagpu_shadow.c` takes a line
number.

**And the first draft of this paragraph got them wrong in exactly the way it was written to
prevent** — it gave 403 and 531, which are the numbers at `6a34c3e`, *before* D1's GLSL lift took
25 lines off the top of the file. D2's review caught it. Naming a revision does not make a line
number safe; it only narrows which tree you have to check it against, and that check still has to
be run.

**The tree had recorded half of this and the half it recorded reads as something milder.**
`tagpu_vk_shadow.c:1039` says `ours` is always 0 from the unit pass, because
`TAGPU_PDHAND.depthOn` lost its producer when 11-5d deleted `tagpu_posedraw_depth_unit`. "Shadows
with no casters" and "no shadow map at all" are different sentences, and only the first was
written down.

#### THE HIRES CHAIN, WHICH IS MEASURED RATHER THAN ARGUED

`ensure()` (`tagpu_hires_draw.c:225`) resolves five entry points through `getgl`. `oglu_load_dll`
has no caller, so `opengl32.dll` is never in the process, so `s_state = 2` and
`tagpu_hires_draw_ready()` is 0 — and `tagpu_native.c:2786` nulls **every** `n2->hires`. The
crowd-static logs of all four A/B arms, on **both** builds, carry the whole story in three lines:

```
hires: armpw -> hires\armpw.glb
hires: hires\armpw.glb loaded, 1577 tris, 6 materials, 10 images, 12 pieces
hires draw: missing GL proc
hires: the replacement pass failed to build (see 'hires draw:' above) -
       replacement units fall back to the engine's own 3DO
```

The file is found, parsed, and thrown away, every session. `nhi` is therefore always 0 and
`hunits[]` (`tagpu_native.c:3228`) is read only by the emptiness test at `:3367`. There is a
second, independent reason the Vulkan side cannot fire: `tagpu_hires_draw` and
`tagpu_hires_depth` have **zero call sites**, and `_depth` is the only writer of `s_hiHave`, so
`tagpu_hires_handover` returns 0 at its only caller `tagpu_vk_hires.c:677`.

#### WHY THE DELETION IS SAFE BY CONSTRUCTION, AND WHAT IT IS NOT

Three independent reasons, none of them timing:

1. **No `renderer=` value reaches the GL lane.** `dd.c:1908-1931`: `opengl`, `openglcore`, `auto`
   and anything unrecognised all land on `vk_render_main`, with a log line for the two spellings
   that used to mean something. 11-2 did that.
2. **`oglu_load_dll` has no caller**, so no GL entry point in any of the four files is
   resolvable. Eighteen references to it exist outside `opengl_utils.c`: its prototype in
   `inc/opengl_utils.h`, and **seventeen comments asserting exactly this**. With comments and
   string literals masked the whole tree has two — the declaration and the definition.
3. **The three features above already produce nothing.**

So the oracle is **0 px on every fixture**, not a difference to explain.

**What it is NOT is a decision about the gate.** Option A keeps `tagpu_vk_shadow.c` and
`tagpu_vk_hires.c` "as the foundation"; the measurement says those are passes that *cannot
currently fire*, and reviving either is a feature landing with a producer to write. Whether 11-6
may close with soft shadows and replacement models dark is the exit condition's meaning and is
**the owner's** — escalation reason 1, named and not answered here.

#### D1 — THE GLSL LIFT, WHOSE GATE IS EXACT

`tools/spirv-gen.py` reads each shader source by preprocessing one translation unit: `src/<name>.c`
by default, or whatever `HEADER_SOURCES` maps the name to. The two doomed files own three of the
Vulkan lane's programs (`shadow_unit`, `shadow_hires`, `hires`), so their GLSL moved into
`src/tagpu_shadow_glsl.h` and `src/tagpu_hires_glsl.h` and `HEADER_SOURCES` gained two entries.

**`SOURCES` and `PROGRAMS` are untouched on purpose.** Removing a name from those stops
generating the `inc/spirv/*.spv.h` that the surviving `tagpu_vk_*.c` files include — the same
trap `inc/openglshader.h` sits in, and the reason for the rule this is the second instance of:
**a file whose only consumer is `tools/spirv-gen.py` is load-bearing and invisible to every
C-level dependency check.** `tagpu_shadow_glsl.h` is now exactly such a file: nothing
`#include`s it.

Both headers sit in `src/` rather than `inc/` because the hires fragment shader pastes five
`TAGPU_GLSL_*` macros out of `src/tagpu_glsl.h` and the generator preprocesses with `-Iinc`
alone. `STR`/`STR2` moved with the shaders for the same kind of reason: the stringify is part of
the shader text, and left behind in the deleted file `uniform vec4 uPiece[TAGPU_HMAXPIECE*3]`
would have reached the generator unsized.

**The gate came back stronger than it had to.** All five `glsl` hashes and all five `words`
hashes are unchanged. Across the thirteen committed headers the only edits are the thirteen
`transform` lines — `tool_hash()` hashes `spirv-gen.py` itself, so touching `HEADER_SOURCES`
invalidates every header by design — and two provenance lines naming the new files. And the
shipped binary is unchanged: `.text`, `.rdata` and `.data` are each **byte-identical** to the
pre-landing clean build, the whole DLL differing in **six bytes** — the COFF `TimeDateStamp`
(`0x88`), the optional header's `CheckSum` (`0xd8`) and the export directory's own timestamp
(`0x166c04`, four bytes into `.edata`). D1 therefore carries no pixel A/B and no review: there is
no behaviour to measure or to read.

#### D2 — THE SHADOW DELETION

`tagpu_shadow.c` and `tagpu_shadow.h` go, with **87 of the tree's 252** narrow GL sites.
`gl-sites` drops to **165 narrow / 234 wide**. The eight live call sites become:

| site | was | is |
|---|---|---|
| `tagpu_posedraw.c`, `tagpu_terr.c` | `s_pub.shadowOn = tagpu_shadow_live() ? 1 : 0;` and a block | `s_pub.shadowOn = 0;` |
| the same block's body | `shadowMat`, `shadowSun`, `shScale`, `penumbra`, `shade` | **no writer left in either file**, and each function opens with `memset(&s_pub, 0, sizeof s_pub)` |
| `tagpu_vk_shadow.c` | `tagpu_shadow_handover(&h, d->frame)` | a named file-local `shadow_handover` stub returning 0 |
| `tagpu_native.c` | `tagpu_shadow_glreset();` | deleted with its dead caller (`tagpu_native_glreset` says so at `:3607`) |

`TAGPU_SHADOWHAND` moved into `tagpu_vk_shadow.h` unchanged, with the measurement above written
beside it, because it is the shape the next producer needs and should not have to reinvent. **The
stub is a function rather than a `return 0;` at the call site**, so a reviver changes one body
and the refusal keeps its name instead of becoming an unexplained early exit.

**The "no writer left" line is the part worth keeping.** The old code published zeros because a
branch never ran; the new code publishes zeros because nothing in the file writes those fields at
all — a stronger statement, and one a grep checks. After the commit, `tagpu_shadow_[a-z_]+`
masked-matches **0 times** in the whole tree.

**AND D2 MADE ONE MORE THING DARK, WHICH THIS SURVEY MISSED AND ITS REVIEW DID NOT.**
`tagpu_shadow_hills` was the only caller of `tagpu_terr_hills_draw`, so that function went from
one call site to **none** — masked counts 3 → 2 across the commit, def and declaration only.
`tagpu_terr.h` still said "its only call site is `tagpu_shadow_hills`, which is itself called
from nowhere", which after D2 is a sentence about a function that does not exist; it now says
there is no call site at all. The consequence worth recording: `tagpu_terr_hills_draw` is the
**only reader** of the heightfield CPU mirror `s_hMeshV`/`s_hMeshI` — **19.3 MB on Two
Continents** by `build_hills`' own arithmetic (`tagpu_terr.c:354`), built unconditionally — so
that memory is now provably unread. It is **not a regression**: a callerless caller read nothing
either, so the mirror has been dead for as long as `tagpu_shadow_hills` has. It is the next
deletion, and `TAGPU_TERRHILLS` has no consumer outside its own header. Named here rather than
left to be re-derived.

It also corrects `tagpu_native.c`'s reason for filling `TAGPU_FXVIEW fv` unconditionally, which
cited a hand-off to `tagpu_shadow_begin` that no longer exists. The fill stays — the gathers read
`fv` and each is gated separately, so the uninitialised-stack defect that comment records (FOUND
2026-09-15) is one re-gating away from returning — but the reason is now the true one.

#### THE D2 MEASUREMENT — 0 px, AND WHAT "0 px" LOOKS LIKE ON THIS FIXTURE

`one-unit`, four interleaved arms, clean-built both sides, `gui.on=1`. Read against 11-5e-2c's
run of the same fixture, where the same four pairings produced a 15×34 box of 258 px.

| | total | minimap | elsewhere |
|---|---|---|---|
| gate 1, worst pair within a run | 1 | 0 | 1 (the cursor at (512,384)) |
| **control** — `main` A vs `main` B | 1 | 0 | 1 |
| **control** — D2 A vs D2 B | 49 | **48** | 1 |
| cross-build, all four pairings | 44–45 | **44** | **0–1** |

**The derived box collapsed to a single pixel — `x 512..512 y 384..384`.** The script takes the
union of the four cross-build bounding boxes outside the minimap, and on 11-5e-2c that union was
the placed unit; here it is the cursor hotspot, the fixture's other known oscillator. So the
honest reading is **nothing outside the minimap and the cursor differs at all**, and both of
those appear in the same-build control at the same or larger magnitude (48 px of minimap drift
between two runs of D2 itself). Census 749 452 non-black px on every arm, zero VK errors on all
four, and `gui: layer ON` confirmed per arm.

That is what a deletion of code that could not execute is supposed to look like, and it is worth
having in the same place as the argument: the argument said 0 px by construction, and the
instrument that found 258 px three landings ago found none here.

#### D3 — THE glTF LOADER AND ITS GL DRAW PASS

**The owner's ruling, 2026-09-19: *"disable gltf for now"*, *"mark gltf implementation: todo out
of scope"*.** That settles the one question this survey could not. `tagpu_hires.c` is deleted
**whole** rather than reduced to its CPU half, and `tagpu_hires_draw.c` with it: **2 588 lines
out, 158 in**, `.text` down **35 648 bytes** and the DLL down **51 712**. `gl-sites` goes
**165 → 31 narrow** and **234 → 98 wide**, in **one remaining file** — `opengl_utils.c`.

**Nothing that was deleted could execute, and there are three independent reasons, any one of
them sufficient.**

1. `ensure()` resolved five GL entry points through a library the process never loads, so
   `tagpu_hires_draw_ready()` was 0 and **`tagpu_native.c` nulled every replacement-mesh pointer
   itself** — immediately after logging the `.glb` it had just parsed.
2. With that pointer NULL on every unit of every frame, the branch that built `hunits[]` and
   posed the mesh never ran, so `nhi` was always 0.
3. `tagpu_hires_depth`, the **only** writer of `s_hiHave`, had zero call sites, so
   `tagpu_hires_handover` returned 0 at its only caller as well.

**Deleting a loader is not deleting one file, and the cascade is the interesting part.** Out of
`tagpu_native.c`: the replacement-mesh branch (42 lines), `hires_pose`, `pmap_for`, `name_eq`,
the `HPMAP` piece-to-primitive cache and its two statics, the `HPOSE_MAX` pose arena,
`hunits[]`, `nhi` and its term in the emptiness test, the `hires` field and its writers, and
`gfx` — whose only reader was that branch. **`gy` was in exactly the same position and the first
cut left it in**: its one reader was `h->shadowDy` inside the deleted branch, so it became a store
nothing loads, which `-Wall` cannot say. The landing's review found it; it is gone now, and with
it the `pu->ground_h` projection that fed it. Found beside it and deliberately NOT touched: `gnd`
has no reader either, but it had none at D3's parent, so it is older than this deletion and is
not this landing's to remove. `cache_gen_check` keeps its call site and loses its
body's only subject: the level-change edge is still the right invalidation point for a
template-keyed cache, so it stays written rather than being re-derived by whoever adds the next
one.

**What is kept, and why each thing is not arbitrary.** `TAGPU_HMAXPIECE` is not about the
loader — it sizes the per-piece uniform array in the shader the **Vulkan** lane still draws, so
`inc/tagpu_hires.h` survives as that one constant and the reason, and changing it fails
`tools/spirv-check.sh`.

**AND FAILING `spirv-check` IS NOT ENOUGH, WHICH THE REVIEW CAUGHT.** The first draft said
`tagpu_vk_hires.c` "sizes the matching block" from the macro. It does not: that block is the
hand-copied literals `VS_SZ 2480` / `VS_PIECE 0` / `VS_GAME 2304`, and the macro appears there
only as the bound on a `memcpy` into it. At 48 they agree exactly — 48 × 12 × 4 = 2304 =
`VS_GAME − VS_PIECE`. Raise the macro to 64, regenerate the SPIR-V as the paragraph above
instructs, and the bound admits `np = 64` while the destination is still 2 480 bytes: the copy
writes 3 072 and runs off the end of `uPiece` into the projection uniforms. So the fix is not a
better comment, it is **a bound**: `tagpu_vk_hires.c` now carries a compile-time assertion tying
the two together, and a bump fails the build (`size of array … is negative`, naming the line)
rather than the frame. Verified both ways — clean at 48, and the error above at 64. `tagpu_vk_hires.c` stays per Decision 1, because `tagpu_vk_shadow.c` and
`tagpu_vk.c` call six of its entry points, and it inherits `TAGPU_HIHAND`, its three sub-types
and `TAGPU_HI_MAXHAND` with the measurement written beside them. Its hand-over is a named
file-local stub returning 0 — the shape D2 established.

**What is genuinely given up is the code, not the feature**: the glTF parser, the piece table,
the material grouping and the COB-driven pose. They are in `git` at this landing's parent, and
the note in `tagpu_vk_hires.h` is the pointer a reviver needs. The feature itself was already
producing nothing.

#### D3'S PIXEL A/B — AND THE OUTLIER GRAB, IDENTIFIED THIS TIME

Four arms, `one-unit`, **interleaved** `mainA brA mainB brB` so the same-build control spans the
same stretch of wall-clock as the cross-build comparison does. Baseline arm is a DLL
section-identical to `main` at the D2 review fix (`.text 0x000d33a4`, 1 517 056 B); branch arm is
D3's (`.text 0x000ca864`, 1 465 344 B).

**These are the RE-RUN's numbers, on the committed review-fix tree** — the fixes took 176 bytes
off `.text`, so the first run's table was not inherited. (For the record the first run gave the
same reading: 48/45/1/44 cross-build, 49 and 45 control, 0 px outside the minimap and the same
single pixel.)

| pairing | total | minimap | elsewhere | box |
|---|---|---|---|---|
| **cross-build** `mainA` vs `brA` | 1 px | 0 | 1 | (512,384) |
| **cross-build** `mainA` vs `brB` | 49 px | 48 | 1 | (512,384) |
| **cross-build** `mainB` vs `brA` | 44 px | 44 | **0** | — |
| **cross-build** `mainB` vs `brB` | 44 px | 44 | **0** | — |
| control `mainA` vs `mainB` | 45 px | 44 | 1 | (512,384) |
| control `brA` vs `brB` | 48 px | 48 | **0** | — |

The difference box derived from the data is **1×1, the single pixel (512,384)** — the same pixel
D2's box collapsed to, and the control puts 1 px in it too, so it is the fixture's oscillating
cursor pixel and not the change. **Outside the minimap and that one pixel: 0 px on all four
cross-build pairings.** The settled census is **749 452** on every arm, as it has been since
11-5e-2c. Zero `VK_ERROR` / validation lines and exactly one `gui: layer ON` per arm.

**AND THE RE-RUN CORRECTED THE COMPARATOR BEFORE IT PRODUCED A NUMBER.** `cmp_x.py` took the
**third** grab as each arm's settled frame, on the assumption that a run settles and then stays
settled, so an outlier could only be an early grab. That is false, and this run is what showed
it: **in three of the four arms grab 3 was the odd one out** and grabs 1 and 2 were
byte-identical, so the old comparator would have computed three of the four cross-build numbers
from outliers. It now picks the settled grab **by agreement** — whichever grab is byte-identical
to another, whatever its index — and exits `GATE 1 FAILED` when no two agree, rather than
choosing a representative from a drifting fixture. The `ta-drive` rule carries the correction.

The one log difference between the builds is the one D3 predicts, and it is worth being exact
about **which** lines, because the first draft of this paragraph named the wrong pair and so
claimed more than the run showed. Each main arm logs two `hires` lines and each branch arm logs
none:

```
hires: keeping the CPU vertex copy for a second lane - 0 model(s) re-read from file
hires: armcom -> no replacement (looked for hires\<name>.glb / .gltf)
```

Those are the **LOADER** declining to find a replacement. `hires draw: missing GL proc` and the
fall-back to the engine's own 3DO — the lines that prove the *draw* path dead — appear **zero
times in all four D3 arms**, and once in each of D2's four. **This A/B did not exercise the draw
path at all**: `one-unit` is an ARMCOM with no replacement mesh, so `tagpu_hires_draw_ready()`
was never reached. The draw path's deadness rests on D2's `crowd-static` run, which did reach it,
and on the static argument above — not on this measurement. Nothing else differs in kind.

**The outlier grab, and what it actually was.** `brB`'s three grabs went 0 px between 1 and 3 and
**97 092 px** between either of those and grab 2 — the rule added to `ta-drive` last landing
(read gate 1 on the settled pair) applied, and this time the outlier was identified rather than
set aside. Grab 2 is the same frame with **every tree missing**: the differing pixels are green in
the settled grabs (mean RGB 15.8/95.3/65.7, 74 distinct colours) and the grey of the terrain
beneath in grab 2. It is not ours to lose — the per-frame census logs `feat=0` in **every** arm,
main included, and the feature pass says so in as many words (`nothing emitted: native.on needs
"wrecks" before we can own the leaf`), so the trees in this scene are drawn by the engine into
its own frame and merely composited. A single frame between two byte-identical ones, in a layer
no Vulkan pass touches, is a capture that caught the engine's own blit half done. **It is not a
D3 effect** — and the honest limit on that statement is that twelve grabs saw it once, which is
too few to put a rate on.

**THREE READINGS OF THIS FRAME HAVE NOW BEEN WRONG, INCLUDING THE ONE THAT REPLACED THE OTHER
TWO.** They are all recorded because the sequence is the lesson.

1. *"A blit caught half done."*
2. After the owner offered *"each time I see it I believe this simply the fog of war being full
   screen"* — fog desaturating terrain and hiding features. **The owner killed this one with a
   domain fact**: grey fog does NOT hide trees, only the black unmapped fog does, and under black
   fog no terrain is drawn. So a frame with grey terrain and no trees is not a fog state TA has.
3. *"The engine's composited layer flickers continuously, and every pixel A/B in this gate has
   passed gate 1 by landing two grabs in the same state."* **D4's review killed this one by
   counting all 36 grabs**, and it was the worst of the three because it was about to steer the
   next gate.

**WHAT THE 36 GRABS ACTUALLY SAY.** Saturated pixels (`max−min > 50`) over every grab of all
three A/B runs:

| saturated px | grabs |
|---|---|
| **74 495** | **33** |
| 42 386 | 1 |
| 34 034 | 1 |
| 2 782 | 1 |

**33 of 36 are bit-for-bit the same saturation state.** There is no continuous flicker; the
fixture is static, and the earlier A/Bs passed gate 1 because the frame *is* stable, not by luck.

**AND TWO OF THE THREE ANOMALIES ARE TORN CAPTURES, WHICH IS READING (1) AFTER ALL.** The rows
that differ:

```
d4_ab1  x_brB_1 vs x_brB_2 : 54 657 px, rows 0..383 BYTE-IDENTICAL, divergence 384..735
d4_ab1  x_brB_3 vs x_brB_2 : 43 455 px, rows 0..391 BYTE-IDENTICAL, divergence 392..735
```

A hard horizontal boundary at row 384 — exactly half of 768 — with the whole top identical is a
**screen tear**. `meas_x.sh` grabs with `import -window` on a live X11 window: no vsync, no
buffer handshake, so a tear is the expected artefact, and 34 034 / 42 386 are *intermediate*
counts, which a flicker between two stable states cannot produce and a part-done update can.

**The third is a different shape and is still unexplained**: `d3_first_ab/x_brB_2` diverges from
row 39 to row 735 — the whole world viewport, not a boundary — and holds only **2 782** saturated
pixels against 74 495. That one is a genuine whole-frame loss of the engine's composited layer.

**THE ONE NEW FACT WORTH KEEPING, AND IT IS ABOUT THE HARNESS, NOT THE RENDERER.** All three
anomalies are in a **`brB`** arm, and `meas_x.sh` runs `mainA → brA → mainB → brB`, so `brB` is
always the **last** arm. Three for three on the fourth launch is arm-correlated, not random. It
is confounded — `brB` is both "last" and "branch" — but the D3 and D4 anomalies happened on
*different branch binaries*, which points at launch order rather than at either build. **Whatever
it is, it is the harness's fourth run, and the honest thing is that nobody has tested that.**

**AND THE GREY-WORLD CLAIM BUILT ON THE HISTOGRAM IS WITHDRAWN TOO, BECAUSE THIS PROJECT HAD
ALREADY REFUTED IT.** The histogram itself is sound — in the world region `y 39..735, x 188..1023`
(n = 582 692 — **that region is the anomaly's own bounding box, not a viewport this project
uses**; under 11-5f's world rect `x 260..1000, y 60..700` the same frame gives 83.8 %, and over
the full frame 77.1 %), 84.3 % of pixels sit at saturation 0–2 under half-open bins and the
median is 0.0 — but that is **our frame measured alone**, and "our lane outputs greyscale while only the engine's content carries
colour" does not follow from it. The engine's frame is substantially grey in places too. An
18-point sample taken earlier in this gate found **5 of 18 points where OURS is MORE saturated
than the engine's** — engine `(43,43,43)` grey against ours `(7,83,43)` green, engine `(31,31,31)`
against ours `(0,128,128)` cyan — and concluded, correctly, that *"we desaturate each pixel" is
refuted*. What is established is narrower and was already written down: **the AGGREGATE
saturation is roughly halved (mean 28.5 vs 14.7), while the structure matches**, which is the
signature of a sub-texel sampling offset on TA's dithered terrain art rather than of a colour
path that drops chroma. **The next test is at the DATA level — one tile through the palette
against the same tile in our atlas — and the per-pixel screen diff must not be re-run**; it is
dominated by dither and alignment noise and cannot separate the hypotheses.

**11-5f now stops for the owner before any fix**, and they review a running game with the shield
off rather than a screenshot — see the [vulkan-only plan](../vulkan-only-plan.html), 11-5f.

#### D3'S REVIEW — 23 FINDINGS, NONE A CODE DEFECT, AND ONE THAT BECAME A BOUND

The dedicated read-only review returned **3 HIGH, 14 MEDIUM, 6 LOW**, verified every mechanical
claim independently (symbol inventories, the moved struct byte-for-byte, the five GL entry
points, `gl-sites`, both binary deltas, spirv currency, the pixel table recomputed from the
grabs) and found **no correctness defect in the deletion**. Every finding was a stale comment or
a wrong statement — which is what this gate keeps producing, and the reason the review is worth
its cost.

**THE ONE THAT WAS NOT PROSE (M1), and the only one that changed the binary's contract.**
`inc/tagpu_hires.h` said `TAGPU_HMAXPIECE`'s second consumer, `tagpu_vk_hires.c`, "sizes the
matching block", and that changing the macro "fails `tools/spirv-check.sh`" — inviting the reader
to regenerate the SPIR-V and consider the job done. It does not size anything: the block is the
hand-copied literals `VS_SZ 2480` / `VS_PIECE 0` / `VS_GAME 2304`, and the macro appears there
only as the bound on a `memcpy` into it. At 48 they agree **exactly** — 48 × 12 × 4 = 2304 =
`VS_GAME − VS_PIECE`, with no slack at all. At 64 the bound admits `np = 64` into a 2 480-byte
destination and the copy writes 3 072, off the end of `uPiece` and through the projection
uniforms. A comment asking the next maintainer to remember this is what was there, and it was
wrong about which file sizes what, so the fix is **a bound rather than better prose**:

```c
typedef char tagpu_vk_hires_upiece_fits[
    (TAGPU_HMAXPIECE * 12 * (int)sizeof(float) <= VS_GAME - VS_PIECE) ? 1 : -1];
```

Verified both ways: clean at 48, and at 64 the build stops with `error: size of array
'tagpu_vk_hires_upiece_fits' is negative` naming the line. (`_Static_assert` is C11 and the build
is `-std=c99`, hence the negative-array idiom.) This is the shape CLAUDE.md asks for — a bound,
checked by construction, not a note asking someone to be careful.

**THE FINDING AGAINST THIS LANDING'S OWN MEASUREMENT (H2).** The paragraph above about the A/B
first said the two `hires` lines that vanish from the branch arms were `hires draw: missing GL
proc` and the fall-back to the engine's own 3DO. The **count** was right and the **lines** were
not: those two appear zero times in all four D3 arms and once in each of D2's. D3's fixture is an
ARMCOM with no replacement mesh, so `tagpu_hires_draw_ready()` was never reached and the two
lines that do vanish are the LOADER declining to find a file. The sentence as written read as
"the A/B watched the dead draw path stop logging", and it did not. Corrected above, with the
limit stated: **the draw path's deadness rests on D2's run and on the static argument, not on
D3's measurement.** Naming the right count with the wrong lines is the subtlest kind of
overclaim, because the number checks out.

**THE FINDING THAT WAS DEAD CODE THIS LANDING CREATED (L1).** `gfx` was not the only local whose
sole reader was the deleted branch — `gy` was too, and the first cut left it in. Its one reader
was `h->shadowDy` inside that branch, so it became a store nothing loads, which `-Wall` cannot
say. Removed, with the `pu->ground_h` projection that fed it. **Found beside it and deliberately
not touched**: `gnd` has no reader either, but it had none at D3's parent, so it is older than
this deletion and belongs to whoever audits it rather than to this landing.

**EIGHT CONTRADICTIONS INSIDE THE TWO FILES THE LANDING WAS EDITING (H1).** The truthful 80-line
block went in at the FOOT of `tagpu_vk_hires.h`; the contract at the HEAD of the same file — and
of `tagpu_vk_hires.c` — still said the bodies belong to `tagpu_hires_draw.c`'s GL program, that
`tagpu_shadow.c` counts the casters, and that `tagpu_hires_depth` is the oracle. All three were
deleted by D2 and D3, so each file stated both things at once. Both contracts are now in the past
tense and point at the foot of the file first. This is the same failure the D2 review returned as
F5, one landing later, which is worth recording as a habit rather than an accident: **a session
adding a truthful block to a file does not thereby read the rest of it.**

**AND A FEATURE PAGE STILL DOCUMENTING A DELETED FEATURE AS WORKING (H3).**
`research/notes/model-import.md` opened "Drop a glTF beside the game and the fork draws it
instead of the unit type's 3DO" with a table pointing at all three deleted files, and was not in
the docs commit at all; so did `gpu-status`'s own G11 half-table ("**Proven end-to-end**") and
two `roadmap.md` G11 rows. All four now carry the owner's ruling and read in the past tense, and
the pages are kept as the design record because that is what a revival needs.

**THE OTHER TWELVE**, each verified against the source before it was touched: five writers of
`s_hiHave` across three functions, not "exactly one" — the claim the argument needs is *the only
assignment of 1* (M2); comments describing the deleted `nhi` term, the deleted hires pass, the
deleted consumer of the pose form, the deleted `s_pmap` cache and the removed `n2->hires = NULL`
(M3–M7); a `glreset` precondition that has now been MET and read as pending (M8); a dead
`#include "tagpu_hires.h"` in `tagpu_native.c`, the exact residue §2.75 is about (M9); a roadmap
range of "44–49 px" that misdescribes a pairing with 1 px and no minimap difference (M10);
"13 and 2 call sites … once each lost one" — `pose_accum_body` has **one** left and `nlog` lost
**four** of seventeen (M11); §2.80's heading still naming only D1 and D2 (M12); a "not covered"
line naming two files this gate has since deleted (M13); nine cross-file comments and one
scenario description naming the deleted files as live (M14); 74 distinct colours in the outlier
grab, not 67 — the first count sampled only the first 20 000 differing pixels and was reported as
if it covered all 97 092 (L2); and the `bt` sentence, both halves wrong (L4).

**NOT FIXED, AND IT CANNOT BE.** `57820b1`'s commit message carries the `.text` figure 35 136,
which is wrong — it is 35 648. The correction is in the notes and in `4ae4e3a`'s message, and
history is not rewritten here to tidy a number, so `git log` keeps the wrong one. Recorded rather
than quietly left (L3).

**THE FIXES CHANGED THE BINARY, so the A/B was re-run rather than inherited.** D2's review fixes
were comment-only and `.text`/`.rdata`/`.data` came out byte-identical, which let that landing
keep its measurement. Not here: removing `gy`'s dead stores took **176 bytes** off `.text`
(`0xca864` → `0xca7b4`). Dead-store removal is behaviour-free in every reasonable reading, and
"every reasonable reading" is exactly what this repo does not accept as a measurement, so the
table above is from the re-run.

#### TWO THINGS D3 GOT WRONG FIRST, BOTH CAUGHT BEFORE THEY LANDED

**The first cut of the surgery mangled three comment blocks.** It located each function's
preceding comment with a heuristic — "walk back while the line looks like a comment" — which cut
three blocks in half and left `/*` unterminated. The compiler caught it (`"/*" within comment`).
The cut was **thrown away**, `tagpu_native.c` restored from a pre-surgery copy, and the surgery
redone with explicit line ranges, each one **printed for inspection and checked for balanced
`/*` and `*/` before anything was deleted**. A heuristic that is right four times in five is the
wrong tool for a deletion, because the fifth is silent.

**And the inventory check earned its place again.** The repo added it after a scripted deletion
once swallowed a neighbour, and it is run against a clean `git archive HEAD` tree rather than
against assumptions: the non-static symbol lists of `tagpu_native.c` and `tagpu_vk_hires.c` are
**identical** to HEAD's, 15 and 6. Two *static* functions did leave the object file — `nlog` and
`pose_accum_body` — and that was checked rather than shrugged at: both are still defined and
still called — `nlog` at 13 sites and `pose_accum_body` at **1** — and gcc inlined each entirely
once its remaining callers were few enough. (The first draft of this sentence said "13 and 2 …
once each lost one": `pose_accum_body` has ONE call site left, `pose_dump`'s, and `nlog` lost
FOUR of seventeen, not one. Counts re-derived with `grep -cE '\bname\s*\('` at both revisions
after the landing's review caught them.) The
compiler-generated clones that vanished from `tagpu_vk_hires.o` (`mem_type.isra.0`,
`mk_buffer.constprop.0`) are the expected consequence of a stub that folds to a constant.

#### D4 — THE LAST GL FILE, AND THE LANE IS GONE

D4 deletes what D1–D3 left: **`opengl_utils.c`** — the loader, the `wgl` bootstrap and the
**82** file-scope `PFN…PROC` pointers it resolved through **59** `xwglGetProcAddress` calls,
which between them were the whole of `gl-sites`' remaining 98 call sites — together with its
header and the three vendor headers only it included — **7 619 lines**, of which
**7 006 are vendor headers** (`glcorearb.h`, `wglext.h`, `khrplatform.h`) that nothing else in
the tree includes.

| out | lines |
|---|---|
| `tagpu/ddraw/src/opengl_utils.c` | 493 |
| `tagpu/ddraw/inc/opengl_utils.h` | 120 |
| `tagpu/ddraw/inc/glcorearb.h` | 5 879 |
| `tagpu/ddraw/inc/wglext.h` | 845 |
| `tagpu/ddraw/inc/KHR/khrplatform.h` | 282 |
| **total** | **7 619** |

Plus four lines out of `render_gdi.c` and thirteen in, so the commit is **7 623 deletions and 13
insertions**. `gl-sites` goes **31 narrow / 98 wide → 0 / 0**, and there is no GL call left anywhere
in `tagpu/ddraw/src` — **which is `gl-sites`' whole file set, and not the same thing as "the
tree"**: its docstring excludes `tagpu/src/**` (the other DLL) and `tools/`, and both still have
GL. `tagpu/src/tagpu.c` carries 5 narrow / 6 wide and `tools/vkcoexist.c` 8 / 19, neither of them
in this DLL, and `inc/KHR/` goes with its only file. `.text` −4 816 bytes, the DLL −7 680.

**`inc/openglshader.h` is NOT in this set, for the third time of asking.** It has **zero**
`#include` directives naming it anywhere in the tree; `tools/spirv-gen.py` is its only consumer,
through `HEADER_SOURCES`, and the Vulkan lane draws one pair out of it — the base blit that puts
TA's 8-bit surface on the frame through the palette. Every C-level dependency check sees an
orphan. The check that does not: walk `SOURCES` + `HEADER_SOURCES` and every `#include`
reachable from them under `-Iinc`. That reaches **62 files**, `openglshader.h` among them and
none of D4's five. *A file whose only consumer is `tools/spirv-gen.py` is load-bearing and
invisible.*

#### WHAT MADE THE DELETION SAFE, ESTABLISHED BEFORE THE EDIT

Not "it compiles" — that is the check this repo has already watched fail, because a missing
non-static function compiles clean under `-Wall`. Four facts, each re-derived:

1. **No live reference outside the set, inside the DLL this builds.** With comments *and*
   string literals masked by `gl-sites.py`'s own masker, `(g_oglu_\w*|oglu_\w*|xwgl\w*|wgl[A-Z]\w*)`
   matches **268 times in four files** at `main` — 130 `opengl_utils.c`, 119 `wglext.h`, 17
   `opengl_utils.h`, 2 `render_gdi.c` — and nowhere else **in `tagpu/ddraw`**, which is 0 at HEAD.
   (This first said "230 … in the repo", which was wrong twice: the count came from a scratch
   script with a looser pattern, and the scope was never the repo. **`tagpu/src/tagpu.c:55` has a
   live `wglGetProcAddress(n)` call** and `tools/vkcoexist.c` has eleven masked matches — neither
   is in this DLL, so the safety conclusion stands, but "nowhere else" did not.)
   `tagpu_overlay.c`, `tagpu_gaf.c` and `tagpu_vk.c` look like callers to a plain grep and are
   **all comments**, which is the trap this repo documents and the reason the masked count
   exists at all.
2. **The two entry points have no callers.** `oglu_init` and `oglu_load_dll` masked-match four
   times between them: two prototypes and two definitions. So `opengl32.dll` is never loaded.
3. **`g_oglu_version` was therefore always the empty string.** It is a file-scope `char[128]`
   with exactly one writer, inside `oglu_init` — so it held its zero initialisation for the life
   of every process, and `strlen(...) > 10` was always false.
4. **The one player-visible consequence was checked rather than assumed.** `gdi_render_main`
   **is** reachable (`dd.c`, `render_vk.c`, `winapi_hooks.c`), so the driver warning can still
   appear. Its parenthetical was already rendering empty; the literal keeps the `()` and drops
   the `%s`, so the text a player sees is identical character for character.

#### VERIFIED BY INVENTORY, NOT BY "IT COMPILES"

`render_gdi.c` is the only edited translation unit, and a missing non-static function compiles
clean under `-Wall` — which is why this repo checks the inventory instead. Built `git archive
main` into a temp tree, compiled the file both sides, compared `nm --defined-only`:

```
main : _gdi_render_main@0
HEAD : _gdi_render_main@0      IDENTICAL
```

and the DLL's export count is unchanged at **22** — the Export Address Table's 0x16 rows.
(This said 44, which is that table counted together with the Name-Pointer table: the same 22
symbols twice.)

**And the player-visible string was checked in the binaries, not argued about.** The two string
tables:

```
main  "-WARNING- Using slow software rendering, please update your graphics card driver (%s)"
D4    "-WARNING- Using slow software rendering, please update your graphics card driver ()"
```

`%s` was always fed `g_oglu_version`, always `""`, so both render `…driver ()` — character for
character the same thing a player saw before.

#### THE TOOL REFUSED ITS OWN FIRST RUN, WHICH IS THE RIGHT FAILURE DIRECTION

The edit script ends with a guard: nothing named `opengl_utils` or `g_oglu` may survive in
`render_gdi.c`. It fired — on the replacement comment, which names both as *history*, because
that is what a tombstone comment is for. The guard now masks comments and string literals first
and checks only what the compiler sees. Worth recording rather than quietly patching: a deletion
tool that refuses wrongly costs one minute, and D3's first cut — a heuristic that was right four
times in five — cost a thrown-away surgery. **Refuse by default; the fifth case is the silent
one.**

#### D4'S PIXEL A/B — AND THE GATE FAILED HONESTLY ON THE FIRST ATTEMPT

Four arms, `one-unit`, interleaved, both arms clean-built. Baseline is `main` at `5392778`
(`.text 0x000ca7b4`, 1 464 832 B), branch is D4 (`.text 0x000c94e4`, 1 457 152 B).

| pairing | total | minimap | elsewhere |
|---|---|---|---|
| **cross-build** `mainA`/`brA` | 45 px | 44 | 1 |
| **cross-build** `mainA`/`brB` | 49 px | 48 | 1 |
| **cross-build** `mainB`/`brA` | 45 px | 44 | 1 |
| **cross-build** `mainB`/`brB` | 1 px | 0 | 1 |
| control `mainA`/`mainB` | 48 px | 48 | **0** |
| control `brA`/`brB` | 44 px | 44 | **0** |

The derived box is again **1×1 at (512,384)**, the fixture's oscillating cursor pixel. **Outside
the minimap and that pixel: 0 px on all four cross-build pairings.** Census **749 452** on every
arm, zero `VK_ERROR` / validation lines, one `gui: layer ON` each, **zero `oglu`/`opengl`/`wgl`
log lines on either build**, and no GDI fall-back on any arm — so nothing regressed into software
rendering when the loader went.

**THE FIRST ATTEMPT FAILED GATE 1, AND THAT IS THE POINT.** `brB`'s three grabs disagreed
pairwise by **54 657 / 11 202 / 43 455 px** — no two of the three agreed — and the comparator
**refused to report any number**, exiting `GATE 1 FAILED … the fixture is drifting, not
settling. Do not read further.` That refusal is the fix made earlier the same day (pick the
settled grab by agreement, not by index) catching a real failure on its first outing. The cause
is the 11-5f instability measured above: the engine's composited layer flickers, and a run can
land three grabs in three different states. **Every earlier pixel A/B in this gate passed gate 1
by landing two of three grabs in the same state**, which is luck the old comparator could not
distinguish from a static fixture. The table above is attempt 2, where all four arms settled.

#### THE RESIDUE D4 DOES NOT CLEAN UP, NAMED WITH ITS COUNT

`render_ogl.c` and `render_ogl.h` went in **landing 11-2**. Tracked content still cites them
**169 matching lines at HEAD** (`git grep -c`, which counts lines and not occurrences) — 63 in
`tagpu/ddraw` across 35 files, 95 in
`research/notes/`, 8 in `tools/`, 3 in `.claude/`. Most of the fork's are present tense:
"render_ogl.c's loop", "called ONCE per frame from the render_ogl.c frame bracket",
"`render_ogl.c:549` asks the LINKER where each attribute landed".

**This is deliberately not folded into D4.** D4 is five files and two lines; an audit of ~170
sites is a different landing with a different risk profile. But D4 is the landing after which
there is no GL lane at all, so "the GL lane" stops being a thing a comment can point at — which
makes this the moment to write the count down rather than let the next session find it.
**It is an input to 11-6's exit condition**: a gate whose title is "the OpenGL lane goes" that
leaves 168 present-tense references to the deleted lane has not finished.

### 2.81 The clean cut — engine compositing is deleted, not levered

**The rule, and it is the code rather than a setting: no pixel on the presented frame originates
from an OBSERVATION of the engine, and there is no way to ask for one.** `tagpu_purevk.on` existed
for a day. The owner's ruling was *"I want pixel compositing with vulkan to be permanently
disabled, not switch or option in engine to bring it back. The code must be a 100 % gone. We only
compare against the second buffer containing the original software rasterizer output"*, with one
condition: *"as long as the original software rasterized is still reachable in another
buffer/texture so that it can be used for reference."* The lever and every branch that read it are
gone with the code they gated.

**Why cut at all.** While any engine pixel can reach the screen, every artifact needs a prior
question answered first — *is this ours or theirs?* — and in the week before this landing that
question was answered wrongly three times (the teal tree shadows, the green vent smoke and the
"missing" units were all one confusion). The screen is now exactly what our own passes drew, so
anything missing is missing **visibly** and is a work item rather than a mystery.

#### What was deleted

Three paths carried engine pixels to the frame. All three are removed at the source:

| path | what went | what is left |
|---|---|---|
| **TA's composed frame**, the frame's bottom layer | `tagpu_vk_surf_record` and its pipeline, descriptor sets, sampler, vertex and uniform buffers and quad; the fork's own `PASSTHROUGH_VERT_SHADER` + `PALETTE_FRAG_SHADER` pair and its generated SPIR-V | the upload. `tagpu_vk_surf.c` is **one phase** now — no `record`, and its header says so |
| **the UI layer** | `tagpu_vk_gui.c` (2897 lines), `tagpu_gui_surf.c` (2518), seven shader programs, `TAGPU_GUIHAND` and the whole twin store | nothing. `tagpu_gui_hook.c`'s op capture survives |
| **the marker pass's captured 8bpp layer** | `TAGPU_MK_TEX_LAYER`, `TAGPU_MARKLAYER`, and `markown`'s context-base swap (`layer_begin`/`layer_end`/`layer_clear`, the double-buffered scratch, `tagpu_mark.on=nocapture`) | nothing. Every other marker is re-derived from engine STATE and drawn as our own geometry |

`tagpu_cursown` went with them — four engine call-site redirects that skipped the engine's cursor
BLIT while ours was on screen. Ours was the composite, so the byte they compare could only ever be
0 for the life of a session, and four patches that can never fire are worse than none.
`render_vk.c` publishes `tagpu_cursown_publish(0)`… and then that call went too, with the module.

**`1791a7e` is reverted.** It stole two branches (`0x46A784`, `0x46A7E0`) to stop the engine's
feature-shadow ALP blend `0x4B8500` landing as teal against terrown's key fill — a defect of the
COMPOSITE, measured at 40 861 px. Under the cut no composite exists, and with `terrown` off the
play defaults there is no key fill either. Measured before reverting: the presented frame **0 teal
and 0 raw key**, the golden source 1280 teal and 625 049 raw key. `tagpu_detour_branch`, the third
stub shape written for it, went back out with it.

#### The UI layer took our own work with it, and that is a property of how it was built

`LAY_FS` sampled the engine's replayed twin **and** G17a's device-resolution sharp layer through
ONE quad and one fragment stage. The engine's half could not be removed from that draw and ours
left in, so the cut took both. What the player has now is **no HUD, no sidebar, no minimap, no
cursor, no dialogs and no shell** — stated here rather than left to be found. The UI returns as a
pass of ours.

**`tagpu_gui_hook.c` survives, and for two reasons.** Capture is not compositing: the op stream it
records is the engine's UI stated *semantically* — this sprite, that string, at these coordinates
— which is what a native UI pass will be built from, and it is the same class as
the reference frame. And `before_flip` is **the only host of
`tagpu_triggers_frame`**, so deleting the file takes every `tacli` verb with it. What it no longer
does is publish: with no consumer, `tagpu_gui.on` is a census-and-diagnostics trigger and is off
the defaults table.

#### The reference: a buffer and a texture, drawn nowhere

`tagpu_surf_capture` takes TA's composed frame on the CPU — **on the GAME thread, at the one point
in the process where it is finished, since 2026-09-20 (§2.82)**; `tagpu_vk_surf_prepare` uploads it into
this frame slot's R8 index image with its 256×1 palette beside it and leaves both in
`SHADER_READ_ONLY_OPTIMAL`. `tagpu_vk_surf_engine_view(slot, &w, &h)` hands the view out — with no
sampler, because the module owns none since the cut took its draw; a consumer brings its own, and
it must be NEAREST (half way between index 7 and index 8 is index 7.5, which is in no palette
entry and belongs to neither neighbour).

**It has no consumer in the tree, deliberately.** The one it had was the composite. The upload
costs one `memcpy` and one `vkCmdCopyBufferToImage` per **changed** frame — the serial gate means
a still screen pays neither — and the alternative is that the next comparison starts with a
rebuild rather than a call.

#### The `*own` levers came off the play defaults, and that is what makes the reference worth keeping

This is the correction to the plan's §6, which calls the engine *"a complete, correct reference
implementation of this game's look, running in the same process, on the same frame, with the same
state."* **In the shipped arm set it was not, and the hole was exactly where you would most want
to check yourself.** `tagpu_terrown`, `tagpu_featown`, `tagpu_fxown`, `tagpu_markown` and
`tagpu_owndraw` stop the ENGINE drawing so ours can stand in its place — right while we composited
over its frame, and afterwards only a way to hole the golden source. The engine's own counters
said it in one line each:

* `TERROWN skip=1 filled=1` — the engine's terrain pass *and* its fog overlay skipped, the
  viewport filled with palette index 254 instead.
* `FEATOWN skip=1` — no tree rasterised.
* `OWND target=all skipped=0 passed=5311` — the unit rasteriser is **not** skipped, which is why
  units *were* in the reference. `tagpu_posedraw_live()` is a hardcoded `return 0`, so the engine
  draws every unit every frame into pixels that were already being discarded.

A `tacli shot` then showed the HUD, the minimap, the units, their bars and the cursor over a
**flat cyan viewport with no terrain and no trees** — not the picture the 1997 rasteriser draws,
but the picture it draws with five of its passes removed.

**They are not deleted.** Arm `tagpu_featown.on` and the engine's features stop exactly as before.
The CPU they save is real and is the whole of what they buy now; it is not the default, because a
golden source with holes in it is the more expensive mistake.

> **`terrown` WENT BACK ON THE DEFAULTS 2026-09-21 BECAUSE THE UI LAYER COULD NOT FIND THE
> VIEWPORT WITHOUT IT, AND ON 2026-09-22 THAT STOPPED BEING TRUE.** The history is kept because
> the mechanism it uncovered is the reason the repair is shaped the way it is.
>
> **What was wrong.** The layer resolves the engine's surface twin through the palette and
> replays it, and with `terrown` absent the picture was a **flat black viewport carrying the
> engine's trees and rocks** — no terrain from either renderer, no unit from either. Following
> that through named three separate things, only one of which was `uVpKey`:
>
> * The twin is a **replay of DESCRIBED draws**, not a copy of the engine's frame. `PK_SEED`
>   crosses with `alen = 0` on purpose and `PK_PIXELS` is dropped by this lane, so the engine's
>   terrain grid blit `0x483FA0` and its unit rasteriser never enter the twin at all — neither is
>   a described op. Its **features do**, because a GAF blit publishes as `PK_SPRITE`. That is the
>   trees, and it is a *provenance* failure: nothing in the channel said which draws were world.
> * The black is the engine's **own whole-surface clear `0x4C6890`**, published as a `PK_BAR` the
>   size of the surface. It is opaque and it covered the viewport. That is a *state* failure.
> * What was meant to re-open the viewport was one **`PK_CLEAR` emitted at the FLIP op**, gated on
>   `tagpu_terrown_fill_seq()` having advanced — and **that gate never fired in play.** It read
>   `nextSeq` for the last flip of the window at publish time, one statement after the marker's
>   own `seq` was taken from the same accessor, so the two were always equal; and at the ~60 flips
>   a second an in-play frame runs at, a publish window holds exactly one flip. The erase was
>   **dead code on the played path**, and `uVpKey` was carrying the viewport by itself. That is
>   why the fix could not be a matter of widening the gate.
>
> **THE THREE FIXES, LANDED 2026-09-22.** They answer different questions and none substitutes
> for another:
>
> | fix | question | what it does |
> |---|---|---|
> | **the world phase** | *may this op be replayed at all?* (provenance) | an op recorded inside one of `DrawGameScreen`'s two world spans is stamped WORLD and `publish` refuses it |
> | **the unconditional erase** | *what does the viewport hold before this window's draws?* (state) | one `PK_CLEAR` over the true viewport rect at the **top** of every in-play publish window |
> | **the clipped chrome fill** | *must the black cover the world at all?* | a whole-surface `OP_FILL` on the frame surface is published as up to four `PK_BAR` bands **around** the viewport instead of one over it |
>
> **The world phase, and the four addresses it rests on.** `DrawGameScreen` paints its world in
> **two** straight-line spans with UI between them — the METAL/ENERGY block and the minimap sit
> *after* the terrain blit and *before* the unit sweep — so the bracket is
> `0x468DB0`…`0x468E3A` and `0x469849`…`0x469F36`, each a 5-byte `call rel32` repointed at a
> thunk that sets a byte and tail-jumps to the engine's own function. Both closers are reached on
> every call of `DrawGameScreen`, so the flag cannot leak out of it; every site is byte-matched
> all-or-nothing at attach and redirecting the SITE composes with whatever is detoured on the
> CALLEE, so `terrown`'s skip of `0x483FA0` still wins and `fxown`'s detour on `0x471F90` still
> runs. The full call table, the branch joins and the negative results are in the
> [engine map](../exe-reverse-engineering.html), *The TWO WORLD SPANS*.
>
> **It fails OPEN**: a build whose bytes differ arms nothing, the flag stays 0, no op is ever
> dropped, and the leak is back — the state every build was in before this. `gui: ARMED …
> worldphase=1` in `tagpu.log` says it took; `tagpu_worldphase.off` is the A/B lever, and the
> heartbeat carries `world=<stamped>/<dropped>/<live>`.
>
> **The erase is at the TOP of the window and this is not cosmetic.** The flip marker is pushed at
> the top of `before_flip`, i.e. AFTER the frame's ops, so an erase emitted there lands on top of
> every engine draw the frame put inside the viewport — and `dedup` makes that total rather than
> intermittent, because it keeps the LAST of two identical ops and the one surviving copy of a
> per-frame draw sits immediately before the marker. **MEASURED 2026-09-22: with the erase at the
> marker, the `PAUSED` banner is in the golden source and absent from the screen; moving the
> erase to the top of the window brings it back.** The control that made that readable was the
> pre-landing DLL, built from the same tree and run in the same instance under `--keep-dll`:
> `PAUSED` draws there, so it is a regression this landing introduced and then closed, not a
> pre-existing gap.
> The gate is arm state — `ret == FLIP_RET_GAME` and `tagpu_packet_pub_level_open()`, both taken
> on the game thread and both static across the frame — and deliberately not a counter the world
> pass publishes, which would be read here a frame late and be down on every 0→1 acquisition edge
> (`tagpu_owndraw.c` ~:210-225 is the precedent). The viewport rect is read only while a level is
> open: `main+0x37E27` keeps the last level's rect in the shell, and an ungated read would punch
> a hole in the MAIN MENU's own backdrop fill.
>
> **MEASURED, `build-facing` on Two Continents at 1024x768, the play defaults, exactly-black
> pixels of the 896x704 viewport in a grab of the game window:**
>
> | arm | viewport black | what is on screen |
> | --- | --- | --- |
> | play defaults (`terrown` on) | **0.03 %** | the whole picture |
> | play defaults + `tagpu_terrown.off` | **0.03 %** | the whole picture — 8898 distinct colours, the same number |
>
> That second row was **80.40 %** before this landing. And the prize that goes with it: with
> `terrown` off, **`tacli shot` has terrain again** — the golden source is a whole 1997 frame,
> not a key-filled viewport. `terrown` is still a play default and still an available
> optimisation; it is no longer load-bearing for the picture.
>
> **WHAT THE WORLD PHASE COSTS, AND IT IS EVERY "LET THE ENGINE DRAW IT" MODE, NOT JUST THE
> MARKERS.** The first revision of this section named only the world-anchored markers; that is the
> most visible case, not the rule. The rule is: **everything the engine draws inside the two spans
> is refused, so any configuration in which one of our passes stands down and the engine's own
> draw of that thing is left enabled now loses it from the screen** — it still reaches the golden
> source. Concretely, with the call site that puts each inside a span:
>
> | mode | what the engine draws | site | what you now get |
> |---|---|---|---|
> | `mark.on=passive`, `nobars`, `noselbox`, `nocursor`, `nodigits` | selection box, health bars, group digit, build cursor, band box | `0x4699EB`, `0x469B8A`, `0x469CF9`, `0x469EC5`, `0x469F1E` | golden source only |
> | `fx.on=passive` | the effect layers | `0x469B22`, `0x469B2C` | golden source only |
> | `sfx.on=passive` | the sfx layers | via `0x471F90` ×10 | golden source only |
> | no `native.on`, `owndraw` also absent | **the units themselves** | `DrawUnit 0x45AC20` at `0x469A00` and `0x469BA3` | golden source only |
> | no `terr.on`, `terrown` also absent | the terrain | `0x483FA0` at `0x468DB0` | golden source only |
> | no `feat.on`, `featown` also absent | the feature sprites | the feature leaf `0x46A610`, called ×3 from span 2 | golden source only |
>
> So `mark.on=noselbox` now means *no selection box on the screen at all*, and an instance with no
> `native.on` and no `owndraw` means *no units on the screen at all* — where before the clean cut
> each meant "the engine's copy instead of ours". (Note there is no `<pass>.off` lever for any of
> these: `tagpu_opt.c`'s rule is that `.off` does nothing while `.on` exists, so the configuration
> is the arm file's **absence**, which is what `--defaults` or a hand-built arm line produces.
> Only `tagpu_worldphase.off` is a real file, and it is this landing's own.) MEASURED 2026-09-22 for the marker row: with `noselbox` and a unit selected,
> the box is in `tacli shot` and absent from the window. The other rows follow from the same
> mechanism and the call sites above; only the marker row was measured.
>
> This is correct by the phase's own argument — these are world-anchored draws, projected through
> the engine's unzoomed eye, and wrong at any zoom ≠ 1 (`markown`'s note calls the engine's box "a
> ghost box at the unzoomed position") — but it is a real change to what those levers are good
> for. **As a way of forcing engine pixels onto the screen, every one of them is now blunted, and
> the lever to use instead is `tagpu_worldphase.off`**, which restores the pre-landing behaviour
> for all of them at once.
>
> **What the leak actually looked like, now that it is gone.** A cross-build A/B at the play
> defaults on the same fixture differs by **36 949 px of 786 432, every one of them on a tree**:
> the baseline painted the engine's 8bpp sprite over ours (119 distinct colours in the differing
> set) and this build shows ours alone (694 distinct colours there), mean |Δ| 1.86. Arm
> `feat.off` and the trees now disappear entirely, where before the engine's stayed.
>
> **STILL OPEN, and not closed by this landing.** `uVpKey` is still wired and still reads
> `tagpu_terrown_filled() ? key : -1`; the two op kinds that could carry raw engine bytes into the
> viewport are a `PK_SEED` that deliberately carries none and a `PK_PIXELS` this lane drops, which
> suggests the rule has little left to protect — but that is still "suggests", not "measured", and
> retiring it belongs with the rest of the `terrown` removal (the fog tick's re-homing, `g_ssTerr`,
> the `tacli` auto-arm). What this landing did delete is `tagpu_terrown_fill_seq()` and
> `g_fillSeq`, which had no reader left once the erase stopped asking.
>
> *(An earlier revision of this line also listed "`OP.seq`'s other readers" as open. There are
> none — both reviewers swept the tree 2026-09-22 and `OP::seq` and `tagpu_terrown_fill_seq` have
> no surviving reader anywhere. `tagpu_packet.c`'s `m->seq` is the command ring.)*
>
> Two residuals the review named, both cosmetic-worst-case and neither fixed here:
>
> - **`tagpu_order.c`'s `bstarted`** infers "this site is already a nanoframe" from a `ptr_ok` on
>   the order node's `N_TARGET`. Nothing is dereferenced, so it cannot fault; but that the engine
>   *clears* the node when the building finishes is `[INFERRED]` — the release path has not been
>   disassembled. A stale in-range target means the ghost for that one site never returns. The fix
>   is that disassembly, not a wider test.
> - **`terrown`'s `terr_fill` writes into the engine offscreen**, and this landing restores it to
>   the play defaults, so that write now happens in every session. It is bounded by the engine's
>   own validated clip rect (the same rect the engine's blits obey), which is a design bound and
>   not a timing one — but the offscreen carries no buffer height, so the literal `cb < 8192` in
>   `tagpu_terrown.c` is the only backstop on the last row. Unchanged by this landing; named here
>   because the landing is what makes it a default again.

**AND IT CLOSED A GAP NOBODY WAS AIMING AT: `renderer=gdi` IS STOCK AGAIN.** `tagpu_owndraw.on`
was the one suppression with no runtime gate — two structure-shadow `je`s flipped to `jmp`s at
`DllMain`, which is why landing 10b replaced them with detoured branches behind `g_ssSkip`
(§2.63). That fix made the flip safe; taking the lever off the defaults means it
does not happen at all. `tagpu_owndraw_init` opens
`if (!tagpu_opt_on("tagpu_owndraw.on")) return;`, so at the shipped defaults **the module writes
no byte anywhere**: not the two rasteriser detours at `0x459830`/`0x459C70`, not `buildfx` at
`0x458DD0`, not the three completed-unit shadow sites, not the structure-shadow pair. Whoever
arms the lever gets all of it back, with 10b's gate under it.

#### And the terrain pass's emit gate went with the composite

`tagpu_terr_gather` refused to emit a single cell without `terrown` installed —
`emit = own || s_over || wasFilled` — and the stated reason was the key test: *"our terrain is
opaque and covers the whole viewport, so any other time it would hide the health bars, wireframes,
build cursor and chat that the composite's key test exists to let through."* With `terrown` off
the defaults that gate turned the ground to clear colour: the heartbeat read `grid=29x23 …
cells=0`, so the 667-cell grid was walked every frame and not one quad came out of it. **Nothing is under us now**, so the gate is deleted; `own` stays and still asks whether to
stop the *engine*, which is a question about the reference and the CPU rather than about what we
draw. Census before: `3 pass(es) drew (terr=0 feat=1 unit=1 fx=1)`. After: `4 … (terr=1 …)`.

#### The review found three more emit gates of the same shape, and a producer I had taken away

The terrain gate above was not the only one. `tagpu_mark.c` refused its bars and digits on
`!tagpu_markown_installed()`, `tagpu_mark_cursor_ours()` folded the same test in (so the build
cursor, the drag band box **and the build ghost** went with it), and `tagpu_order.c` refused the
whole order overlay on it. Every one said *refuse rather than double-draw*, and every one was
answering a question about the composite. All three are deleted.

**AND `markown` IS A PRODUCER, WHICH IS WHY TAKING IT OFF THE DEFAULTS WAS A BIGGER CHANGE THAN
IT LOOKED.** `mark_orders` is the sole caller of `tagpu_order_snapshot`, and the hook-8 stub the
sole caller of `tagpu_packet_pub_font_snapshot` — so with the redirects uninstalled the order
arena was never filled and the packet carried no font at all: no order markers, no route dots,
no target circles, no `ShowRanges` labels, **no text anywhere**, whatever the gates said. The
census said it in one field and I did not read it: `4 pass(es) drew (terr=1 feat=1 unit=1 fx=1)`
with no `mark=`. So `tagpu_markown.on` is back on the defaults table, as the producer, and it is
the one `*own` lever that is.

| | before | after |
|---|---|---|
| census | `4 pass(es) drew (terr=1 feat=1 unit=1 fx=1)` | `5 … (terr=1 feat=1 unit=1 fx=1 mark=1)` |
| the packet's font | no `packet: font … copied` line at all | `rows=11 yoff=1 glyphs=95 bytes=1612 dropped=0` |
| on screen | no bar, no digit, no cursor, no order overlay | bars over every owned unit; a control group's digit under the bar |

**What it costs the reference, and it is the only hole the defaults leave in it.** While the
marker pass draws it asks `markown` to skip the engine's own, so `tacli shot` shows a 1997 frame
without its bars, digits, order markers and build cursor. That is why the skips stay dynamic
rather than becoming unconditional: `tagpu_mark.on=passive` hands every one of them back with
the redirects still installed, and that is the arm for a reference-quality capture.

#### The structure slant shadow is NOT drawn on this lane, and the clean cut did not do it

The same review read `n2->shadow = tagpu_owndraw_structshadow_ours() && …` (`tagpu_native.c`) as
a fourth gate of that shape, and the first term is indeed 0 for the life of every session now
that `owndraw` is off the defaults. **But nothing on the Vulkan lane draws the slant at all.**
Landing 11-2 deleted this pass's GL tail and *"the selection rects, the slant and silhouette
shadow draws, the wire and ghost passes, the depth pre-pass"* went with it; `tagpu_vk_unit.c`
contains no `slant` anything and the geometry build's shadow branch is four blank lines.
**Checked on `main` as well — the gap predates the clean cut and no defaults change made it.**
The stale term is simplified away because it said nothing true; the shadow is an open item for
whoever ports that draw, and the field is kept as the record of what such a pass must ask.

#### Two halves of the UI hand-over that outlived their other half

Neither is wrong at runtime — no engine memory, no pixel, no wrong state — and both are recorded
here rather than deleted, because deleting them is a landing with its own review.

* ~~**The publisher has no producer gate.**~~ **WITHDRAWN — the entry was true of the gap
  between the clean cut and the UI-layer rebuild, and was carried past it.** It read that
  `g_gui_draw` had one writer in a deleted file, so `publish()` returned on its first line and
  the 16 MB arena was never written. `tagpu_gui_surf.c` exists again and its trigger poll is
  that writer (`g_gui_draw = on`); the queue is filled, the arena is written, the drain the
  entry said a consumer would have to bring is in the same file, and the publisher's heartbeat
  reads `published=4653791 bytes=902287148 draw=1` on an ordinary boot. `tagpu_gui.on` is a play
  default and buys the UI. The entry is kept struck through rather than deleted because three
  other notes quoted it.
* **The minimap handshake has no asking half.** `tagpu_gui_set_want_minimap` and
  `tagpu_gui_set_minimap_have` are callerless, so `tagpu_gui_want_minimap()` answers 0 for the
  process, `tagpu_packet_pub.c` never copies the three 126-px surfaces or the TNT picture, and
  `want_minimap_watchdog` can never fire. Nothing starves: the publisher's own test is
  `if (tagpu_gui_want_minimap())`, so not asking costs the copy and nothing else.

#### What it measured

`feat-forest`, Two Continents, `--los 0`, 1024×768, **the play defaults** (not
`tagpu_defaults.off`). Three settled grabs of each, all three identical.

| | |
|---|---|
| **the presented frame** | the engine's chrome — the top bar (22 528 px) and the sidebar (95 488) — is the seam's clear colour **exactly**, `0 px` of anything else. **0 teal and 0 raw key** anywhere in the 1024×768 frame. |
| **the golden source** (`tacli shot`) | **0 raw key, 0 teal**, 170 distinct colours in the viewport and 59 in the sidebar: terrain, water, trees, units, health bars, minimap, resource bar and cursor. A complete 1997 frame. **[Measured with `markown` off the defaults, which is what this landing shipped for one commit. Since `7a933a0` put it back as the producer, the engine's bars, digits, order markers and build cursor are skipped while ours draw — `tacli shot` shows the frame without them, and `tagpu_mark.on=passive` is what reproduces the reading above.]** |

Health: `viol=0 crcbad=0 trunc=0 overrun=0`, no `ErrorLog.txt`, sim ticking **30.00/s** at speed
10, `applied 10 of 10, 0 failed`.

**The blind shell works and is now measured twice.** The plan named this the one untested
assumption worth spending the first ten minutes on: with menus undrawn, can the harness still get
into a game? Yes, and the whole path runs — `tacli scenario load` clicked `SINGLE`, `Skirmish` and
`Start`, set both toggles, reached a live world and applied all ten entities, with the census
reading **`0 pass(es) drew`** for every frame of the shell. `tacli ui` reads the engine's own
gadget array and clicks at the coordinates that array reports; `_scn_wait_live` reads `tagpu.log`.
No part of the route reads a pixel.

#### What the frame shows where nothing draws

The lane's clear colour, which is **black** (`s_clear` = 0,0,0) unless `color=` in `tagpu_vk.on`
says otherwise. It was magenta — the louder answer, and what the lane had from its bring-up — until
2026-09-23, when the owner ruled for black: the clear is what a player sees past the map's edge at a
zoom-out wider than the map, where the engine would leave black. An operator who wants the sentinel
back arms `tagpu_vk.on=color=255,0,255`. Measured the same day at `z = 0.25` with the eye pinned at
(0,0) on Two Continents, the play defaults on: 0 magenta pixels of 786 432, the off-map field black.

#### Still open, and the first one is a correctness bug in the instrument

* ~~**The capture runs on the RENDER thread and nothing sequences it against the game thread's
  draw**, so the reference can hold a torn frame.~~ **FIXED 2026-09-20, and the tear it warned
  about was real: 223 of 16 500 reads at that site came back torn (1.35 %).** The copy runs on the
  game thread now, from the packet publisher's `after_draw` observer on `DrawGameScreen 0x468CF0`,
  past the flip — §2.82 has the mechanism, the ownership rule the hand-over needs, and the
  numbers. What that hook does NOT cover is the shell, which never calls `DrawGameScreen`.
* **The engine still rasterises a whole frame nobody sees**, and that is now deliberate twice
  over: the reference needs it, and stopping it was always the last step. Nothing here measures
  what it costs.
* **Three contracts have a producer and no consumer** — named so they are not read as dead code:
  `tagpu_vk_surf_engine_view` (above); `tagpu_gui_set_want_minimap` / `..._set_minimap_have`,
  which still gate the packet publisher's 252×252 TNT minimap picture and its three live minimap
  surfaces, so nothing asks and nothing is paid for; and `tagpu_gui_installed`.
* **A build gate caught a file the deletion had left in `thread-split.allow`**
  (`src/tagpu_cursown.c`), and two "clean builds" before it were a grep pattern that did not match
  `Error 2` — the DLL under them was forty minutes old. Check the exit status, not the output.

### 2.85 The strip — nothing in the build names a GL object, and no setting names GL

**Landed on the branch `worktree-build_ghost`, 2026-09-23, by the owner's ruling: no aliasing, no
backward compatibility.** 11-5 D4 deleted the last GL *file*; this removed what the lane left in
everything else, measured by a scan that masks comments and string literals and lists every
compiled identifier naming GL (`glreset`, `ogl`, `wgl`, `opengl`, `vbo`/`vao`/`fbo`, `palTex`).

- **The renderer switch knows `auto`, `vulkan` and `gdi`.** The `openglcore` latch
  (`opengl_core`), the `direct3d9` arm that sent an old ini to GDI and the `opengl` log line are
  gone; any other value is logged once and runs Vulkan. Measured: `renderer=openglcore` logs
  `ddraw: renderer=openglcore is not a renderer (auto, vulkan, gdi) -- using vulkan` and comes
  up on Vulkan in 210 ms; `renderer=gdi` logs no `vk:` line and answers `tacli ui` and `tacli shot`.
- **Dead state:** `opengl_y_align`, `render.tex`, `shader` (the libretro GLSL option),
  `nonexclusive`, `TAGPU_FRAME.surface_tex`, the posebake GL generation and its key fields, the
  `vbo`/`vao` fields of `TAGPU_PBGEOM`/`TAGPU_PBMAT`, the `palTex`/`scafTex` parameters of the
  terrain, feature and effects hand-overs (every caller passed 0), `TAGPU_GUI_WHY_GLCTX`, the UI
  drain's skip-to-reset latch and its `skipped=` heartbeat field.
- **The ten `*_glreset` functions**, whose root `tagpu_native_glreset` had no caller; its banner
  said to delete the cascade with the last GL file.
- **Files:** `inc/openglshader.h`, the `tagpu.dll` companion (`tagpu/src/tagpu.c`, `tagpu.rc`,
  `tagpu/Makefile` — nothing loaded it), the upstream C++Builder config GUI (`config/`, which
  wrote `renderer=opengl` and could not be built here), `tools/gl-sites.py`, `tools/vkcoexist.c`
  and `vkcoexist-pixels.sh`. The hook skip-list no longer names `opengl32`, `libgallium_wgl` or
  `libglapi`. `opengl32.dll` is not mapped in the game process (`/proc/<pid>/maps`, in game);
  the one GL-named library there is `libGLX_nvidia`, which is NVIDIA's Vulkan ICD on the
  reference setup (its `nvidia_icd.json` names it).
- **What players see:** the release `ddraw.ini` said `renderer=openglcore` and its README asked
  for OpenGL 3.3; both name Vulkan now. The generated ini offers no GL renderer, no `shader=` and
  no `nonexclusive=`. About forty `tagpu.log` messages that compared against "the GL twin" name
  the gather or the requirement instead; every prefix and format argument is unchanged.
- **Tools:** `tacli glshot` is gone. `uiwalk` loses `--layer`/`--vk` and the parity code, and its
  loading stop waits for `vk: up in N ms` — it waited for `GL CONTEXT CHANGED`, which nothing
  logs, so every load ran out its 150 s. `promo/shoot.sh`, `promo/survey-map.py` and
  `tascene ab` grab the client window (`import -window`, 8-bit PPM for `ab`).

**Not closed here:** about 800 comment mentions of GL remain in the DLL sources, most of them the
history the comment review kept. `tascene ab`'s browser half drew no terrain in headless Chrome
(the capture half is right: same view, same Commander, same wreck), so its percentage was not a
parity figure; the viewer feeds the instanced terrain shader since 2026-09-23 (worktree-camera_zoom,
[tascene-design](../tascene-design.html) *Gaps*), and `ab` has not been re-run since. `big-battle` shows `unit=0` in the census from ~frame 3300 on `main` as well
(`TAGPU_PD_MAXHAND`): the hand-over cap, which §2.86 removes.
`uiwalk --game-only --screens-only --cycles 1` is clean except two stops of the cycle, and
`main`'s DLL gives both identically: `VISUALS#1` misses the `FPS` gadget by 23 px, and
`ARMMAIN2#1` has 574 unexplained px in (502,356)-(520,392), a unit-sized box at the viewport
centre.
