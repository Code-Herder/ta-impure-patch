# Log — the Vulkan-only plan, gates 1–3 and landings 5–7

Landing records; the plan is [vulkan-only-plan](../vulkan-only-plan.md).

*A frozen record: nothing new is written here. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.35 What the Vulkan lane draws in the SHIPPED configuration — measured, and it is the UI alone

**MEASURED 2026-09-16**, DLL `644ce2e`, instance launched with `--defaults` (the player's
configuration, not a bench one), `one-unit` on Two Continents at 1024x768, the reference setup's
4070, `tagpu_vk.on` armed live. This is landing 1 of [vulkan-only-plan](../vulkan-only-plan.html) and
it writes no code: the whole deliverable is this section.

**Every Phase G figure was taken under `tagpu_defaults.off` + `ss=1` + `gui.on=mmbase`.** Nobody
had started the lane in the configuration the patch ships in. The `opt:` line for this run is the
shipped one —

    opt: play defaults ON (no tagpu_defaults.off): native=all wrecks owndraw=all terr terrown
    feat featown fx sfx fxown mark markown order ghost zoom vpwide gui classicpp weapons

— all eighteen passes `ARMED`, `ghost: ARMED alpha=0.40`, no `tagpu_ss.off` so supersampling is
at its shipped `2x`.

**The result: the Vulkan window presents the UI and nothing else.** Of 786 432 px at 1024x768,
**630 589 are the lane's clear colour** — the whole viewport. The side panel, the top and bottom
bars, the minimap and the resource readouts are all there and correct; the world is not drawn at
all. The lane is not broken and nothing crashed: every world pass **stood down on purpose**,
which is the behaviour each of them documents.

#### The two causes, and they are independent

**1. The Classic++ restored atlases have no CPU mirror.** Four passes log the same refusal, once
each, and then draw nothing:

    vk: unit: the GL twin is drawing through the Classic++ restored atlas and that surface has no
        CPU mirror - the Vulkan edition draws nothing this session rather than draw a different
        picture from its own oracle
    vk: terr: ... the Classic++ restored tile atlas ...
    vk: feat: ... the Classic++ restored atlas ...
    vk: fx:   ... the Classic++ restored atlas ...

`classicpp` is a **play default**. So in the shipped build this refusal is the normal case, not
an edge one. The GAF and tile atlases already have opt-in CPU mirrors for the indexed path
(`tagpu_gaf.c`, `tagpu_terr.c`'s `s_mirrorWant`); the *restored* surfaces the restorer paints do
not, and a Vulkan pass cannot read a GL texture.

**2. The cast-shadow map has casters this lane cannot draw**, and it takes two more passes with
it:

    vk: shadow: the GL map holds 1 caster(s) this lane has no copy of (0 of them the unit pass
        carries) - the native 3DO stream and the replacement meshes are still to port. Nothing
        drawn while there are, and the passes that sample the map stand down with it
    vk: unit: the GL twin drew these units against a cast-shadow map and the Vulkan lane has none
        this frame - nothing drawn while that is true
    vk: terr: the GL twin is reading the Classic++ cast-shadow map and this frame's Vulkan map was
        not drawn (its casters are not all on this side of the seam yet) - nothing drawn rather
        than a different picture from our own oracle

**Cause 2 alone is enough to blank the world.** With `classicpp.off` armed live the FEATURES and
their shadow splats appear — trees, rocks and their shadows over the clear colour — and the
terrain and the commander still do not. So turning Classic++ off buys the feature pass and
nothing else.

#### The stand-downs are SESSION-LATCHED, and the word in the log is literal

*"draws nothing **this session**"*. Each refusal sets a `s_said*`-style latch and the pass does
not come back when the condition clears: after `classicpp.off` and then `classicpp.off=off`, the
feature pass that had been drawing was dark again and stayed dark. A player who toggles Classic++
mid-game does not get the world back; only a relaunch does. This is worth stating because it
makes the lane's behaviour under a *live* lever different from its behaviour at launch, and every
Phase G measurement armed its levers before the lane came up.

#### What this does to the plan's landing order

[vulkan-only-plan](../vulkan-only-plan.html) had `tagpu_vk_mark.c` at landing 3 and the restorer at
landing 5. That order is wrong: **a restored-atlas CPU mirror and the 3DO / replacement-mesh
caster stream are prerequisites for any world pixel at all** in the shipped configuration, and
the mark pass draws over a world that is not there. They move to the front.

**The build ghost is moot for now.** `tagpu_vk_unit.c:1316`'s `otherDraws` stand-down — the one
a build ghost or a frame past `TAGPU_PD_MAXHAND`'s 512 units trips — was never reached, because
the unit pass refuses on the atlas mirror several checks earlier. It stays on the list; it is not
the first thing in the way.

#### Not covered

* **One map, one scenario, one resolution, one GPU, one OS.** Two Continents, `one-unit`,
  1024x768, the reference setup's 4070 under Wine. S3 has still never run.
* **The shell was not walked** in this configuration — only in game.
* **`ss=2` was on and is therefore untested as a difference**: with no world drawn there is
  nothing for the supersample factor to change. The gap named in the plan (the lane has no
  offscreen world target) is unaffected and still unmeasured.
* **`tagpu_gui.off` was not tried** here; the plan's reading of `tagpu_vk_gui.h` stands
  unmeasured.
* **No pixel A/B was run.** This section is an inventory of what draws, not a parity figure.

### 2.36 The Classic++ restored atlases, mirrored for Vulkan — gate 2 of the Vulkan-only plan

**MEASURED 2026-09-16.** [gpu-status](../gpu-status.html) §2.35 found that in the shipped
configuration the Vulkan lane drew the UI and nothing else, because four world passes each stood
down on the same refusal: *the GL twin is drawing through the Classic++ restored atlas and that
surface has no CPU mirror*. This closes that for three of them — features, effects and terrain.
The unit pass is not here; see *Not covered*.

**A Vulkan pass cannot read a GL texture**, which is why the refusals existed at all. The fix is
the mechanism `tagpu_gaf.c` already had for the UI atlas — `tagpu_gaf_atlas_mirror_rgb` and
`_rgb_step`, whose only caller was `tagpu_gui_surf.c:2476` — extended to the world atlases.

#### What each pass needed

**Features and effects** are `TAGPU_GAFATLAS` instances, so they ask for the same mirror on the
same arm beat as their indexed one, behind the same latch and gated additionally on
`tagpu_classicpp_assets()` so a session with Classic++ off never pays the second 16 MB. One
read-back step per published frame, in the publish rather than the arm beat, because it is a
`glReadPixels` off an FBO and that is the render thread with the context current.

**Terrain needed a different mechanism.** Its existing mirrors are *"the buffer the
glTexImage2D was handed, kept instead of freed"*, which cannot work for `s_rgbTex` — the
restorer paints that on the GPU. And `tagpu_terr.c` had none of the entry points, because
`glReadPixels` is not among the fork's own globals and has to be fetched. So
**`tagpu_gl_rgba_readback()`** was added to `tagpu_gaf.c`, where those entry points are already
resolved: a caller-owned FBO made once, the attachment dropped on every path so a caller's
texture is not kept alive past a delete, `GL_PACK_ALIGNMENT` and the bound framebuffer restored.
It knows nothing about an atlas.

**The content key is the part that decides whether this works at all.** `tagpu_gaf.c`'s own step
records why, from a landing it cost two rounds: *a serial that is not the CONTENT's serial
uploads once and then misses everything after it.* Terrain keys on the restorer's paint count
**plus** the row count, because `ensure_atlas` can grow the atlas without a paint landing in
between.

#### Three ordering faults, all found by measurement

The terrain half did not work when first written, and the A/B is what said so. Each fault was
hidden behind the one before it, and all three are the same shape — **a refusal placed above the
code that would satisfy it**:

1. The read-back was gated on `s_job`, which `tagpu_terr.c:1109` frees the instant the restore
   COMPLETES (*"the texture is ours"*). It therefore zeroed the mirror at exactly the moment the
   twin became fully painted. The twin is `s_rgbTex` plus `s_rgbState` (1 restoring, 2 complete);
   the job is only meaningful during state 1, and the key takes a `-1` sentinel once there is no
   counter left to read.
2. The restored image's **resize** sat after the refusal that tests it. `prepare` returning 0
   skips everything below, so the image was never created — measured as `img=0 view=0` against a
   perfect hand-over (`rows=2720 serial=1`).
3. The **upload** sat after the refusal too. Image created, `have=0`, the same deadlock one level
   down.

The refusal now sits after the uploads, so the data lands and the test passes in the same frame.

**AND THE REFUSAL TESTS THE VIEW, NOT THE HAND-OVER.** A first version tested `t.atlasRgb`,
which meant a resize failure left binding 42 naming the **indexed R8** image while the refusal
passed — the shader would have sampled R8 through an RGBA sampler and drawn a wrong picture
instead of standing down. Testing the view and its contents makes a failure self-correcting: the
binding falls back to the indexed view, which keeps the descriptor valid, and the branch goes
unreachable again. The placeholder comments the G19e author left at each of these bindings
(`tagpu_vk_feat.c:773`, `tagpu_vk_fx.c`'s 43, `tagpu_vk_terr.c`'s 42, and `tagpu_vk_unit.c`'s 43
which is still one) say *"this is the binding that stops being a placeholder"*; three of them now
have.

**The refusals are also no longer latched.** §2.35 measured what *"draws nothing this session"*
costs: a pass that refuses once stays dark for the process even after the condition clears. These
conditions clear by themselves as the restorer paints, so the flag now gates the log line and not
the refusal.

#### What was measured

One-unit and fx-lasers on Two Continents, 1024x768, `ss=1`, one pass armed at a time, the
reference setup's 4070:

| pass | fixture | result |
|---|---|---|
| features | feat-forest | **0 px of 786 432**, 243 695 non-black a side |
| effects | fx-lasers | **0 px of 786 432**, 2 557 non-black a side (3 452 on the first run: the duel's ink moves with what is in flight) |
| terrain, Classic++ **off** | one-unit | **0 px of 786 432**, 630 606 non-black a side |
| terrain, Classic++ **on** | one-unit | **5 px of 786 432**, worst channel 1, 630 774 non-black a side |

**Every row above was taken twice: once before the landing review and once after its fixes**, and
the two agree — the same counts, and the terrain 5 at the same pixel with the same two values. The
figures quoted are the post-fix ones. That is the point of re-measuring rather than carrying the
first numbers forward: three of the five findings changed code on the path these captures go
through.

**The terrain 5 px are not a regression, and the previous-build A/B is what established that.**
On `bfbe8b6`'s DLL the same fixture gives **0 px with Classic++ off** and **no picture at all**
with it on — the pass stood down. So the indexed path is unchanged and the 5 px belong to the
newly-enabled restored path, which previously produced nothing to compare against. They are
deterministic: three runs, same count, same first pixel (721, 173), GL (60, 60, 60) against
Vulkan (59, 59, 59). That rules out a stale or racing mirror, and the magnitude rules the mirror
out anyway — a wrong read-back differs in thousands of pixels, not five. One level on all three
channels, arising only when Classic++ turns `uRestored` and `uLit` on together, is float rounding
in the lit path. 0.0006 % of pixels at one level is inside the bar already accepted for restored
art ([renderers](../renderers.html) 4c: max 1 level on 0.0012 % of bytes).

**The features 0 px covers less than it looks.** `restoreglsl: feat: lazy restore armed` and
nothing after it — the restorer had painted no feature frame in that fixture, so the twin was
alpha 0 throughout and both lanes fell back to the palette per texel. It proves the upload
corrupts nothing, that the `uRestored == 1` branch is reachable on both sides, and that the
fallback through it is identical. It does not prove restored feature colours match.

#### The landing review, and the two faults no picture would have shown

Five findings at `medium` on the accumulated diff, all five verified against the code and acted
on. **Two of them were invisible to every measurement above** — the pixels were already 0 px and
the build was already clean — which is the case this gate is worth writing down:

1. **A heap overflow in the terrain mirror's allocation, CONFIRMED.** The growth test read
   `s_rgbMirrorRows > s_atlasH` — the rows last *read* against the atlas's height — so it fired
   only when the atlas SHRANK, and the zero path sets those rows to 0 on the very map change that
   precedes a growth, so it could never fire at all. The buffer then stayed at the first map's
   size while `rows = s_atlasH` grew with the second and `glReadPixels` wrote past the end: a
   1000-tile map followed by Two Continents is **4.7 MB allocated and 23.7 MB written**. The test
   is now the ALLOCATED height (`s_rgbMirrorCap`), and the new buffer is allocated before the old
   one is freed so a refused `calloc` leaves a working mirror rather than none.
2. **A 16 MB-per-frame re-upload, for ever, CONFIRMED.** `doRgb` compared a stored row count
   against the hand-over's, but what was stored was the rows *sent* — forced up to `atlasDim` —
   which can never equal the shelf-bounded rows the producer publishes. So the comparison was
   always unequal, the upload ran every frame on both the feature and the effects pass (**~32 MB
   a frame between them**), the staging buffer was never given back, and a `mk_buffer` failure
   would eventually take the pass down for the session. The serial alone decides now; the field
   was renamed (`s_arReq`) because a name that says "rows" while holding "rows I asked for" is
   how this happened.
3. **An image lost with its memory still bound, CONFIRMED.** A partial `mk_image` failure nulled
   the image and view but left the allocation, so the next `kill_image` would `vkFreeMemory`
   memory with a live image bound to it. `kill_image` on that path, which is what
   `tagpu_vk_terr.c`'s `shared_resize` already did for the identical case.
4. **The restored resize ignored its return value, and ran before the bounds.** `shared_resize`
   returns 0 when a retire is still outstanding — one at a time, by design — and the call site
   dropped that, leaving the PREVIOUS image standing with `view && have` set. The refusal then
   passed and the upload memcpy'd `s_rgbAtlas.w * s_rgbAtlas.h * 4` bytes out of a mirror holding
   `atlasRgbRows`: a read past the mirror when the deferred size was the larger, the wrong rows
   sampled when it was not. Two map changes inside one turn of the slots is what it takes. It now
   sits with the other two shared images, after the bounds block, with its return read the same
   way; `t.atlasRgbRows` is bounded against `t.atlasH` in that block (the producer's bound is a
   bound only while both files are read together); and `doRgb` additionally requires the image's
   own `w`/`h` to match, because *"I asked for this size"* is not *"the image is this size"*.
5. **The helper duplicated the step it was extracted from.** `tagpu_gl_rgba_readback` re-stated
   `tagpu_gaf_atlas_mirror_rgb_step`'s body line for line — ~35 lines, a second place for the
   pack alignment, the saved binding and the dropped attachment to be got wrong. The step now
   CALLS it. That needed one addition to the helper: an optional `status` out-parameter carrying
   the `glCheckFramebufferStatus` value, because an **incomplete** framebuffer is a permanent
   property of the texture and both callers latch on it, which a 0/1 return cannot tell them.
   Terrain latches on it too now, and drops the mirror and its rows when it does — a mirror
   nothing will read again is up to 23 MB held, and leaving the ROWS standing would be worse than
   the memory, because the hand-over publishes on `rows > 0` and the consumer would go on drawing
   the last read-back while the restorer painted past it.

**And the prose was a finding of its own.** Each pass's *"WHAT IT DOES NOT DO"* header still
opened *"CLASSIC++'s RESTORED ATLAS IS NOT MIRRORED"* with the new paragraph stacked underneath
it, so a reader who stopped at the first sentence got the opposite of what the code does. The
leading claim is rewritten in each of the three files rather than contradicted, on the rule this
fork already applies to its counters (`gafstale` → `gafnoplane`): **the statement changes with the
meaning.** `tagpu_vk_unit.c`'s copy is left alone and is still true.

One statement in the code was wrong in the other direction and is also corrected: the terrain
refusal's comment claimed the frame that uploads is lost and the next one draws. It is not —
`shared_upload` sets `have` as it RECORDS the copy, into this frame's own command buffer ahead of
this frame's draw with a barrier between them, so the upload and the draw are the same frame. The
log bears it out: **zero refusal lines** in the post-fix runs, on a fixture whose twin restores to
completion.

#### Not covered

* **The unit pass.** Its mirror is not in this landing: it cannot be verified until the caster
  stream lands, because `tagpu_vk_unit.c` stands down on the cast-shadow map first — *"the native
  3DO stream and the replacement meshes are still to port"* — so an unmeasured consumer would be
  an unfinished unit of work. `tagpu_vk_unit.c`'s binding 43 stays a placeholder.
* **Restored FEATURE and EFFECT colours**, for the reason above: the lazy queues had painted
  nothing in either fixture. Terrain is the only pass here whose restore runs to completion.
* **One map, one resolution, one GPU, one OS**, and `ss=1` — which the A/B requires and the patch
  does not ship.
* **The 5 px are attributed, not traced.** The argument is from magnitude and from which uniforms
  turn on together, not from a line of shader arithmetic.
* **Findings 1, 2 and 4 are fixed but not reproduced.** Each needs a map change (two of them in
  quick succession, for 4), and the A/B fixtures are single-map by construction; the arguments are
  from reading the code against the allocation and the serial. An in-process map change brings the
  whole Vulkan lane down and back up, so the two-map test is a fresh lane rather than a resized
  one and does not exercise the retire path the fourth finding is about.

### 2.37 The caster census, measured before it was ported — gate 3a of the Vulkan-only plan

**MEASURED 2026-09-16.** The Vulkan-only plan filed landing 3 as *"the caster stream — the native
3DO stream and the replacement meshes, so the cast-shadow map can be drawn on this side of the
seam and the passes that sample it stop standing down."* The exit condition was right and the
mechanism named in it was wrong, in both directions: one of the two things it named cannot
happen at all, and the thing that was actually blocking every world pass was not a caster.

#### What the census actually holds

`tagpu_shadow.c` counts, as `otherCasters`, every caster it drew into the GL map that the Vulkan
hand-over carries no copy of. Four kinds were named. Reading the code against the numbers:

| caster kind | status |
|---|---|
| the posed bodies | **covered** since G19e — `tagpu_vk_unit_cast`, subtracted as `tagpu_vk_unit_casters()` |
| the native 3DO stream | **cannot occur** — dead code, below |
| the replacement meshes | **live and uncovered** — gate 3b |
| the heightfield with no mirror | uncovered by design; an out-of-memory path both G19e reviewers found |

**THE NATIVE 3DO STREAM IS DEAD CODE.** `tagpu_shadow_unit` has exactly one call site
(`tagpu_native.c`, the caster loop) and it sits behind `if (skip || firstv[i + 1] == firstv[i])
continue;`. `nv` is 0 at the top of that unit loop and is not incremented anywhere inside it —
the first increment is `emit_fx_model`, *after* `firstv[nu] = nv`. So `firstv[i+1] == firstv[i]`
for every unit, the caster draw never runs, the body `glDrawArrays` beside it draws zero
vertices, and `otherCasters` never counts a native-stream caster. The file says as much in its
own words three hundred lines earlier — *"an ordinary unit contributes no vertices either now"* —
since G16 step 8 made the posed program the path. **There is nothing to port.** The code is
marked where it stands rather than deleted; deletion is the plan's landing 11.

#### What was actually blocking every world pass

With the restored atlas **off** and four posed units on screen under Classic++ soft shadows, the
census closes on its own: `otherCasters - ours == 0`, no refusal on any pass, the shadow pass
uploads its caster mesh and draws, and the terrain and unit passes both draw into the Vulkan
frame. With the restored atlas **on**, the same fixture: *"the GL map holds 1 caster(s) this lane
has no copy of (0 of them the unit pass carries)"*.

The difference is not a caster. The unit pass stood down on **the Classic++ restored atlas**
several checks before it reached its casters, so `tagpu_vk_unit_casters()` answered 0 whatever
the casters were, and the census refused every frame with a unit on it — which stood the shadow
map down, which stood the terrain down. §2.35's *"the lane draws the UI and nothing else"*, read
from the other end: **one missing mirror, propagating through three passes.**

So gate 3a is the half §2.36 deferred — the unit atlas's restored twin — and the caster stream
proper is gate 3b.

#### The mirror, a fourth time

Gate 2's mechanism applied unchanged: `tagpu_render3do.c` gains `_mirror_rgb_want` / `_step` /
`_mirror_rgb` beside the indexed trio, asked behind `tagpu_classicpp_assets()` so a session with
Classic++ off never pays the second 16 MB and stepped once per published frame on the render
thread; `tagpu_posedraw.c` publishes `atlasRgb` with **its own rows and its own serial**; and
`tagpu_vk_unit.c` takes a second RGBA8 image at binding 43, which stops being the placeholder the
G19e author left there.

**Gate 2's five findings were applied here before they could be made again** — the serial ALONE
decides the upload, the rows are bounded against the atlas's own square in the consumer, the image
is built before the refusal that tests it, and a device refusal of the image is non-fatal. The
refusal also stopped being a statement about the session: it said *"draws nothing this session"*
and now says *"until the read-back produces rows"*, which is a few frames.

#### THE MEASUREMENT WAS WRONG FIRST, AND EVERYTHING ELSE FOLLOWS FROM THAT

The first version of this landing measured **0 px** and was wrong in four independent ways at
once. The reason it could be is one line in `tagpu_native.c`:

```c
if (fxOn || sfxOn || featOn || terrOn || markOn) {
    …
    tagpu_rglsl_step();          /* the restorer paints HERE, and only here */
}
```

**A unit A/B armed with `native.on` alone never steps the restorer.** The twin is created, the
job is armed, `restoreglsl: unit: lazy restore armed` appears in the log — and nothing is ever
painted. The `uRestored == 1` branch then samples alpha 0 on both lanes and both fall back to the
palette per texel, so the two agree perfectly *about a branch neither of them exercised*.

**`mark.on` is the lever that fixes it**: it is on that list and it is the only one with no
Vulkan pass of its own, so the restorer paints and the unit pass is still the only pass drawing
into the Vulkan frame — which the capture guard requires. With that one file added, the same
fixture went from **0 px** to **2 126 of 2 132 unit pixels differing**.

**The check that the twin actually painted costs nothing and there is no log line for it.**
`tagpu_restoredump.on` writes the twin once the job goes idle; failing that, run the fixture twice
with `assets=1` and `assets=0` and `cmp` the two **GL** captures. Byte-identical means the
restorer painted nothing and the run says nothing about restored art. §2.36's feature and effects
figures carry exactly this caveat, and now it is known why.

#### Four filter faults, each hidden behind the one before

With the fixture honest, the residual came apart in order. Every figure is `selbox-facings` on
Two Continents, `ss=1`, the unit pass's own A/B (`tagpu_posedraw.ab`):

| what was wrong | why | worst channel |
|---|---|---|
| the image was `dim × rows` | the UVs are normalised against the whole atlas (`tagpu_gaf.c`: `u0 = x / a->dim`) and the GL twin is `dim × dim`, with no scale uniform between them — so `v = 1.0` meant row `rows`, off by `dim/rows` | — |
| binding 43 had the **indexed** sampler | the restored twin is the only texture this shader reads that holds true colour instead of palette indices, so it is the only one GL filters (`GL_LINEAR`) | 155 → 124 |
| the image had **one mip level** | `tagpu_gaf.c` gives a mipped twin `GL_LINEAR_MIPMAP_LINEAR` to `GL_TEXTURE_MAX_LEVEL`; a 32-texel cell lands on a ~23 px sprite, so LOD ≈ 0.5 and GL blends levels 0 and 1 in ordinary play | 124 → 122 |
| the mirror's levels 1+ were **one paint batch stale** | `twin_mips` runs at the top of a frame, `tagpu_rglsl_step` paints in the middle, the read-back runs at the end — so the read-back that first sees a new paint count sees level 0 fresh and the levels above it as they were *before*, then latches `mirroredPainted` and never looks again | 122 → **7** |

**The mip levels are read back, not re-derived.** A blit chain on the Vulkan side would be this
fork guessing at `glGenerateMipmap`'s reduction, and the guess would be a per-driver difference no
note could pin down. Reading GL's own levels makes the two byte-identical by construction — the
rule the whole seam runs on.

**The fourth one was found by looking at *where* the residual was rather than trying another
filter.** 422 of 581 differing pixels were more than 8 levels apart and the Vulkan side showed
flat greys where GL had colour: the signature of sampling a mip built from different texels, not
of a filter setting. Three filter guesses had moved the number by 33 levels between them; reading
the difference image moved it by 115.

#### Anisotropy: the one difference that is not a bug, and the owner's call

With everything above carried across, the residual was **566 of 2 132 unit pixels at 1024×768 and
413 of 1 528 at 640×480, worst channel 9**, ink identical. Setting the twin's anisotropy to 1 on
**both** lanes gives **0 px of 786 432 at 1024×768 and 0 px of 307 200 at 640×480**.

So the whole remainder is the anisotropic filter. Both APIs leave sample placement to the
implementation and the same driver does it differently for each — there is no Vulkan sampler
state that reproduces GL's, and none is coming.

**The owner's decision (2026-09-16): play keeps 4× on both lanes; the A/B is taken at `aniso=1`
as a stated substitution.** It is the same shape as G19f's walk, which compared the lanes rather
than running `strict`: the oracle still catches every porting mistake and excludes only the one
thing the specs leave free. `aniso=` is a `tagpu_classicpp.cfg` knob, default 4, and **both lanes
read it** — `tagpu_gaf.c` applies it and publishes what it actually got, `tagpu_vk_unit.c` builds
its sampler from the same number, and a knob changed mid-session makes the two disagree, which is
caught: a sampler cannot be rebuilt mid-frame for the same reason a shared image cannot, so the
pass compares the published ratio against its own and stands the frame down.

`samplerAnisotropy` is enabled on the device for this, exposed as `anisook`/`maxAniso` beside
`flipok` and `zclipok` — the first core feature bit the seam asks for rather than an extension.

#### What was measured

| configuration | result |
|---|---|
| pre-gate-3 DLL, Classic++ art on | **no picture at all** — the pass refused for the session |
| `aniso=1`, 1024×768 | **0 px of 786 432**, 2 128 non-black a side |
| `aniso=1`, 640×480 | **0 px of 307 200**, 1 525 non-black a side |
| the 4× play default, 1024×768 | the pass **draws**: 566 of 2 132 unit px, worst channel 9, ink identical |

The first row is what this landing changed: the same fixture drew *nothing* before it.

**AND ALL THREE ROWS WERE RE-TAKEN AFTER THE RE-REVIEW'S FIXES, to the same numbers** — 2 128 and
1 525 non-black a side, 0 and 0, and the same 566 at worst channel 9. That re-run is not a
formality here: the `s_arReq` fix makes the partial-row upload REACHABLE for the first time, so
every figure above it had been taken with the whole square going up every time. The picture is
the same either way, which is what it should be, and now it is measured that way rather than
assumed. The short-chain refusal is evidenced from the other side by the same run: the Vulkan
column is 2 128 non-black, not 0, so the refusal did not fire on a healthy session.

#### What the RE-REVIEW found, and the one it found in this page itself

Six commits went to a dedicated reviewer after the fixes above, because the fixes were larger
than the landing they were correcting. Nothing was wrong in the part that was expected to be:
all 25 `mk_image` / `img_barrier` / `copy_rect` call sites carry the right level counts, the
`samplerAnisotropy` feature survives every rung of the device retry ladder, and the `mippedN`
content key converges after exactly one extra read-back rather than spinning. What it found:

* **THIS PAGE CARRIED §2.35, §2.36 AND §2.37 TWICE, and one of the two §2.37s was the draft the
  fixture discovery had already invalidated.** The docs commit was a character-offset splice that
  re-appended 1 298 lines instead of replacing 108 — visible in `git diff` as an insertion with
  **zero deletions**, which is impossible for a section that was rewritten. A reader hit the stale
  figures first. **This is the third time this exact thing has happened on this page** (§2.34's
  own text records the other two), and the check that catches it every time is the one this
  landing skipped: regenerate the wiki and read the headings of the page you just wrote.
* **`s_arReq` held the rows SENT, so the partial-row upload was unreachable.** The whole-square
  rule forces the first send to the atlas's full height, so the count latched at `atlasDim` and
  every later frame's shelf-bounded count compared as a shrink: 21 MB memcpy'd per serial change
  — which is most frames while the restorer paints — and 21 MB of host-visible staging reserved
  per slot in a 32-bit address space. **This is gate 2's finding 2, one atlas down**, and
  `tagpu_vk_feat.c` stores the published rows for exactly that reason and says so in its own
  declaration. This pass's comment claimed the same discipline while the code did the opposite.
* **A SHORT MIP CHAIN WAS TO BE DRAWN RATHER THAN REFUSED, and it would have been drawn by
  destroying a live image mid-frame.** The producer handed over "the levels that were actually
  read", on the reasoning that the consumer should build a shallower image rather than one with
  an undefined level in it. Both halves are wrong: GL still filters the twin to its own
  `MAX_LEVEL`, so a shallower Vulkan chain is a *different picture* wherever a unit is minified,
  which is ordinary play — the same thing the anisotropy check beside it stands down for — and a
  depth that moves makes `atlas_rgb_build` `kill_image` an image the other slots' submitted
  command buffers still name, with no fence between. **That is gate 2's confirmed use-after-free
  in a second place.** The fix is at the producer: a chain that did not come back whole is
  reported as no mirror, the consumer already draws nothing without one, and because `mip` is the
  atlas's compile-time depth every publish that carries rows now carries the same depth — so the
  rebuild is unreachable by construction rather than rare.
* **`mk_image`'s failure label left the VIEW as it found it.** `vkCreateImageView`'s out-param is
  undefined on failure and every caller's `kill_image` destroys the view on the strength of it
  being non-NULL. `atlas_rgb_build` had a second `kill_image` to close its own call site; the
  other six were open. One line in the label closes all seven and retires the workaround.
* **A documentation claim about how to make the restorer paint was too absolute.** The skill note
  and the roadmap said `tagpu_rglsl_step` is called from `tagpu_native.c` inside
  `if (fxOn || sfxOn || featOn || terrOn || markOn)`, so that `native.on` alone paints nothing.
  There is a **second caller**: `tagpu_gui_surf.c:2465` steps the restorer whenever the UI atlas
  has a job of its own and nothing else stepped it this frame. The practical advice survives —
  see the corrected wording there — but it is advice from a fixture, not a guarantee from the
  code.


#### And a verification pass over the fixes, because this lane has a record

The four fixes went through a second, narrowly-scoped read — not another landing review, a check
that each fix does what it claims and introduces nothing — on the strength of this lane's own
history: a re-review has found a defect *inside* a fix three rounds running. All four verified.
What it returned instead was three comments asserting the pre-fix world, a guarantee that was not
where it looked, and one hazard:

* **`s_arReq`'s declaration still said "the rows LAST SENT"** — the exact wording the fix's own
  commit message pillories — and the producer's level loop still argued for the shallower image
  that finding 4 overturned. A comment left asserting the opposite of the code beside it is how
  the next reader inherits the bug.
* **The `mk_image` guarantee was a property of the CALLER, not of `mk_image`.** Its three *early*
  returns still left the view — and two of them the memory — as the caller found it, so "every
  out-param is NULL on this path" held only where a `kill_image` happened to run first. All four
  exits now null all three, which is the kind of guarantee a new call site inherits.
* **`mip` IS NOT IMMUTABLE, and the fix's stated invariant said it was.** `r3d_init` reassigns it
  on every GL context reset and the restore demotes it to 0 on a GL with no `glGenerateMipmap`.
  The fix survives on *ordering* — both writes land strictly before any publish carrying rows —
  but the argument had to be rewritten to rest on the invariant that actually holds. That is
  *Fixes must be safe by construction* applied to the argument rather than to the code.
* **AND THE SAME PAIR WAS A HEAP OVERRUN THIS LANDING HAD CREATED.** The mirror is deliberately
  never freed, so it outlives changes to the atlas it mirrors — and all three `memset`s plus the
  read-back sized themselves off the *current* `dim`/`mip`. A buffer allocated at `mip == 0`
  (16 MB) against a `mip` later raised back to 2 is a 21 MB write into it: ~5 MB past the end.
  **The mip chain is what made this expressible** — before this landing every one of those sites
  was the same constant `dim * dim * 4` — so it is this landing's hazard, not a pre-existing one.
  The buffer now records the shape it was allocated with, every write is bounded by that pair
  rather than by the live one, and the read-back refuses a frame whose atlas changed shape under
  it **without latching**, because the demote is re-applied at the top of the next restore and a
  latch would cost the mirror for ever over one frame. Reaching it needs a GL with no
  `glGenerateMipmap` and two context resets; the fix does not rest on that being rare.

The 1024×768 A/B was re-run after all of it: **0 px of 786 432, 2 128 non-black a side** — the new
refusal does not fire on a healthy session, which a guard this close to the hot path has to be
shown rather than argued.


#### Not covered

* **The replacement meshes — gate 3b, and not an optional case.** A tacli instance ships
  `hires/armpw.glb` **active** (`hires/off/` is a parking directory, not a lever), so the census
  refuses **1** caster with one Peewee on screen and **16** on the 257-unit `crowd-static`. Every
  world pass stands down on those frames.
* **The 4× difference is bounded but not traced.** 9 levels on ~27 % of unit pixels is what two
  fixtures at two resolutions showed; no attempt was made to derive it from either driver's
  sample pattern, and none is planned.
* **One map, one fixture, two resolutions, one GPU, one OS,** and `ss=1`, which the A/B requires
  and the patch does not ship. The 0 px is at `shadows=0`; with the cast-shadow map on, the
  soft-shadow PCF adds the 1 px §2.37 recorded before the fixture was fixed, and that pixel is on
  both builds.
* **The terrain and unit passes cannot be A/B'd in the same run** once shadows are on: terrain
  needs the unit pass drawing for the census to close, and two drawing passes make the lane refuse
  the capture. The chain closing is evidenced by the absence of every refusal plus the guard's own
  message naming two passes; the pixels are one pass at a time.
* **THE BANDWIDTH HALF OF THE `s_arReq` FIX IS ARGUED, NOT MEASURED.** What is measured is that
  the picture is unchanged with the partial path reachable. That the pass now sends the shelf's
  rows rather than the whole square on every serial change — 21 MB of memcpy and 21 MB of
  host-visible staging per slot — is read off the code and off `tagpu_vk_feat.c`, which has done
  it that way since gate 2. This pass logs no staging figure, and adding a counter to prove it
  would be instrumentation to leave behind.
* **THE SAME VIEW HAZARD IS STILL OPEN IN THE THREE SIBLING PASSES.** `tagpu_vk_feat.c:374`,
  `tagpu_vk_fx.c:344` and `tagpu_vk_terr.c:409` each `return 0` on a failed `vkCreateImageView`
  without nulling the out-param — and they leak the image and its memory besides, which the unit
  pass's label does not. It is the same one-line class fixed here and it was left alone
  deliberately: those files are outside this landing's diff, so no reviewer has read them in this
  landing, and widening a diff past what was reviewed is how an unreviewed change reaches `main`.
  Named here so it is a known debt rather than a silent one.

### 2.38 The replacement meshes' casters, drawn by Vulkan — gate 3b of the Vulkan-only plan

**MEASURED 2026-09-17.** Gate 3a left one caster kind uncovered and gate 3b is it. A tacli
instance ships `hires/armpw.glb` **active**, so one Peewee on screen made `tagpu_shadow.c`'s
census refuse **1** caster and a 257-unit crowd **16** — and a refused census stands the shadow
map down, and the terrain and unit passes down behind it.

#### What it is, and what it deliberately is not

`tagpu_vk_hires.c` draws those silhouettes into the cast-shadow map and **nothing else**. The
bodies stay with `tagpu_hires_draw.c`'s GL program: a glTF unit shaded per pixel with normal maps
is not what blocks the lane, the census is. That is the whole difference between this file and
its 1 400-line siblings.

Three parts, in the order they had to be built:

1. **`tagpu_hires.c` keeps the CPU vertex copy.** It freed `m->v` the moment the VBO had it —
   right while GL is the only consumer, wrong the moment a lane that cannot read a GL buffer
   needs the same triangles. The sizing written for this gate two days earlier said the vertices
   came "out of the static buffer"; **that was wrong and the code was checked rather than
   remembered.** The case worth the work is asking LATE: the lever can appear after the models
   are uploaded and freed, so the ask forces a re-read from file rather than answering NULL for
   the session.
2. **`TAGPU_HIHAND` records what `tagpu_hires_depth` drew, as it draws it.** `castSkip`, a VAO
   that would not build and a zero-count group each drop a unit from the GL map, and a second
   walk is free to disagree about any of them while both look right. It publishes only if EVERY
   caster the pass drew is in it: a record *short* of the GL map is worse than none, because the
   census counts the GL draws and a lighter map would satisfy it with fewer casters and come out
   **lit where the oracle has shadow**.
3. **The pass, and the census becomes a sum** — `tagpu_vk_unit_casters() +
   tagpu_vk_hires_casters()`. Both count the same way and both can only under-count, so the sum
   stays comparable to `otherCasters` by the same construction G19e established.

#### The albedo is deferred, and that is checked rather than hoped

The fragment shader's depth path is three lines: sample `uAlbedo`, `discard` if the alpha cutout
fails, return. One sampler read, one branch that can discard. The hand-over refuses any frame
carrying a group with `cutoff >= 0` (`cutoutSeen`), so that branch cannot be taken — which makes
the 1×1 white stand-in **not an approximation of the albedo but exactly equivalent to this path**.
The other six samplers the shader declares are untouched there and need a valid descriptor and
nothing more. Every material of the shipped `armpw.glb` is `alphaMode OPAQUE` (6 materials,
checked in the file), so the refusal has nothing to fire on today and is there for the mesh that
is not shipped yet.

#### What was measured

`hires-one` on Two Continents, 1024×768, `ss=1`, the unit pass's own A/B, `aniso=1`:

| configuration | result |
|---|---|
| before this landing, mesh active | **no picture at all** — the census refused every frame |
| mesh active, shadows on | **1 px of 786 432**, worst channel 1, 380 non-black a side |
| mesh **parked**, shadows on | **the same 1 px, at the same (603, 377), with the same two values** |
| mesh active, shadows **off** | **0 px of 786 432** |

The second and third rows together are the point: the pixel is there with **no replacement mesh
in the session at all**, so it is the soft-shadow PCF's and not this landing's. No refusal of any
kind appears in the log of the measured run.

#### AND THE SENSITIVE FIXTURE FOUND A DEFECT THAT IS NOT THIS GATE'S

`hires-one` draws 380 unit pixels whether shadows are on or off, so it barely tests a caster
silhouette at all. `crowd-static` — 257 units, 16 casters — draws ~220 000, and there the same
A/B is **not** clean:

| crowd-static, 1024×768 | differing | worst channel |
|---|---|---|
| mesh active, shadows on | 393 | 158 |
| mesh parked, shadows on | 435 | 158 |
| mesh active, shadows off | 267 | 158 |
| mesh parked, shadows on, `assets=0` | 164 | 176 |
| mesh active, shadows off, `assets=0` | **65** | 176 |

**Every row has the same first differing pixel, (285, 33).** It survives parking the mesh, turning
shadows off, and turning the Classic++ restored atlas off entirely — so it is neither gate 3b's
nor the restored path gate 3a built, though that path amplifies it from 65 px to 267.

**It is edge coverage, and that is measured rather than guessed.** Of the 65 pixels in the
cleanest row, **65 lie on a GL colour edge** — all of them — and in 26 the Vulkan value is
*exactly* a neighbouring GL pixel's. The differences are isolated singletons scattered from
x 201–870 and y 33–726, in both directions (Vulkan brighter in 27 of 65), and at (336, 69) GL has
blue (15, 39, 151) where Vulkan has grey (75, 75, 75) — two different UNITS winning one pixel.
Both lanes use `LESS`, so it is not a compare-op mismatch. What is left is the two rasterisers
disagreeing about which triangle owns a pixel whose centre an edge passes through: a 1-ULP
difference between two compilations of the same GLSL moves an edge across a sample point, and
with four units no edge lands on one while with 257 units 65 do.

**This is the same SHAPE as the anisotropy residual §2.37 escalated, and it is not yet the
owner's call because it is not yet traced.** It is 0.031 % of drawn pixels, bounded, symmetric,
and independent of everything this gate and the last one built — but **gate 3a's headline "0 px"
was taken on a four-unit fixture and could not have seen it**, and that is the honest correction
this section makes to the one above it. What it needs is its own landing: the cause is attributed
from the difference image, not traced to a line of arithmetic.

#### Not covered

* **The caster silhouette itself is not verified pixel-for-pixel.** The gate's exit condition —
  the census closes and the passes stop standing down — is met and measured. But the A/B that
  could see a *wrong* silhouette is the terrain pass's, and terrain cannot be A/B'd while the
  unit pass draws, because the census needs the unit pass drawing and two drawing passes make the
  lane refuse the capture (§2.37 records the same limit). The evidence here is the census closing
  plus the unit pass matching; it is not a picture of the shadow.
* **The edge-coverage defect above is characterised, not fixed, and not traced.**
* **One map, one resolution, two fixtures, one GPU, one OS,** and `ss=1`.
* **The bodies are still GL.** This gate ports the casters; a replacement mesh's own pixels are
  drawn by `tagpu_hires_draw.c` on both lanes and are not in the unit pass's picture at all,
  which is why the non-black count falls from 789 to 380 when the mesh is activated.

### 2.39 The UI markers, drawn by Vulkan (`tagpu_vk_mark.c`, `tagpu_mark.c`'s draw list) — landing 5 of the Vulkan-only plan

**MEASURED 2026-09-17.** The last engine-anchored layer in the viewport: health bars, group
digits, order markers and their `ShowRanges` labels, the build-cursor footprint, the drag band box
and the captured post-fog layer. §2.34 took the UI's own surface; this is what is drawn *over the
world* and *under* nothing.

**The captured post-fog layer no longer exists.** The clean cut (`a2b1333`, 2026-09-20) deleted
`TAGPU_MK_TEX_LAYER` and the `markown` capture behind it, so the list below is one kind shorter
today. The pass draws the selection rects (depth-tested), the order art (route dots and waypoint
sprites, the pass's one blended draw), lines and labels,
the health bars, the group digits, and last the cursors — the build square and the band box —
which are now the one draw with the fog off. `mark.on=nocursor` hands the cursors back to the
engine and fills nothing of ours. The tables record the pass as measured on 2026-09-17.

#### It is seven draws, not one, and that is why a draw LIST crosses

Every other ported pass hands over buckets and counts. This one cannot: `tagpu_mark_render` issues
seven draws in the engine's own order — order triangles, order lines at `ss` line width, order
labels, the health bars, the group digits, the post-fog layer, then the cursors — each with its
own `uText`/`uFog`, and the last two with fog forced off because the engine draws them after its
fog overlay and never darkens them. A consumer that re-derived which buckets were non-empty could
disagree with the pass that drew them, so `TAGPU_MKHAND` carries `TAGPU_MKDRAW { first, count,
lines, text, fog, tex }` records built **as the GL draw issues them**, and `mk_push` mirrors the
VBO byte for byte so every `first` recorded is the one GL used rather than a number re-derived
from the counts.

Nothing else needed a new mirror: the vertices are `tagpu_mark.c`'s own arrays, the captured layer
is already `TAGPU_MARKLAYER`'s CPU bytes, the text atlas was crossed in G19d
(`tagpu_text_atlas(&gen)`), and the palette, fog grid and fog LUT were crossed for §2.30–2.33.

#### The lever it owed, and paid before it was written

Every restored-art A/B on this plan arms `mark.on`, for a reason that has nothing to do with
markers: `tagpu_rglsl_step` runs from `tagpu_native.c` only when one of `fx|sfx|feat|terr|mark` is
armed, and `mark` was the only one of the five with no Vulkan pass of its own — so it stepped the
restorer while leaving exactly one pass drawing, which is what the capture requires. Porting it
made all five drawing passes and the recipe stops working. `tagpu_rglsl.step` was the
replacement: `tagpu_rglsl_step_forced()` polled it on a 30-frame cache and stepped the restorer
while arming no pass at all, with a call-count compare so a frame already stepped was not stepped
twice.

**BOTH LEVERS ARE RETIRED as of 11-5e-2 (§2.74)** — `tagpu_rglsl_step`, `_step_forced` and
`_calls` went with the GL restorer, so `tagpu_rglsl.step` is an inert file that logs nothing when
touched. The Vulkan restorer is stepped from `tagpu_vk.c` inside the frame's command buffer and
needs no lever. The paragraphs above are kept because the TRAP they describe outlives the
mechanism: a restored-art A/B must prove the twin was painted, and a clean run does not.

#### The fixture is the expensive part, and the four earlier runs failed on it

`tagpu_mark_render` returns before it draws anything when every bucket is empty, so the GL half of
the A/B never reaches the disk and there is nothing to diff. Four automated attempts ended there
identically. What the pass actually needs is listed in the `ta-drive` skill; the short form is that
**six of the seven draw kinds each have a separate gate**, and arming `mark.on` opens none of them:

| kind | what makes it non-empty |
|---|---|
| health bars | a unit of the watched player on screen, `damagebars` on |
| group digits | `u->squad` non-zero — `ctrl+<n>` on a selection, nothing else |
| order lines | `tagpu_order.on` **and** SHIFT physically held (the engine's driver at `0x469BFC` is shift-gated) **and** an order that does not complete — a `patrol`, not a `move` |
| order triangles | the marching route dots, which the engine draws only at `flag == 1`: the **hovered** unit. Game speed 1, or a slow unit walks out from under the pointer between `tacli roster` and `pmove` |
| labels | the `ShowRanges` console cheat, typed — `tacli switches` does not reach it |
| cursors | a **held** drag (`down:lbutton`, move, no release) or a build placement |
| post-fog layer | `mark.on=nocursor`, the one window in normal play that still fills it |

#### Two defects that the health bars alone could not show

The first A/B on this pass carried `bars=3` and nothing else and measured **0 px**. Extending the
fixture to six kinds took it to **4 066 px**, and two independent faults came apart behind it.

**The order lines rasterised under the wrong rule.** The pipeline declared
`VK_DYNAMIC_STATE_LINE_WIDTH` and never chained `VkPipelineRasterizationLineStateCreateInfoEXT`,
so Vulkan used its default mode where GL's non-antialiased lines follow the diamond-exit rule.
On 436 segments of route line and range circle that is **4 900 px** the Vulkan lane lit and the GL
twin did not, with the GL half a near-perfect subset of the Vulkan one. `tagpu_vk_fx.c` has
carried `VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT` for the same reason since G19e and this pass
did not copy it. The line pipeline is now built **only** when the device gave us `bresenhamLines`,
so `prepare`'s refusal is a refusal and not a fallback.

**The text atlas was uploaded every frame and never bound.** The GL twin feeds ONE sampler —
`uLayer` — from TWO textures, swapping the bind on unit 0 between the captured layer and
`tagpu_text.c`'s coverage atlas. Vulkan has no per-draw texture bind and this pass had one
descriptor set, whose binding 40 was always the layer. So every text draw sampled the layer — on a
frame with no captured layer, the 1×1 `0xFF` stand-in — `texture(uLayer, vUV).r` was 1.0 for every
fragment, the `< 0.5` discard never fired, and each label and digit came out a **solid filled quad**
in its vertex colour: **3 891 px**. There are now two descriptor sets per slot, identical but for
unit 0, and `record` picks one per draw. A draw naming a texture the hand-over did not bring is
refused outright, because binding 40 falls back to whatever image exists rather than leaving a
hole — the same bug by a second road.

#### And the frame is upside down — the whole lane, and the A/B could not see it

**[FOUND BY THE OWNER, LOOKING AT THE SCREEN, 2026-09-17.]** With the labels finally drawing, the
Route D window showed them mirrored top-to-bottom. It is not the text: **every ported pass draws
the whole frame vertically flipped**, and it has been true since G19e.

**This is not a fact nobody wrote down — §2.28 states it, and states it as correct**: *"Both
halves of this A/B are upside-down pictures of the world, and that is correct: the GL twin draws
into the world FBO, whose clip-space +1 is the bottom of the screen (the composite quad turns it
over). They are upside down identically, which is the only thing the comparison asks."* Every word
of that is true **of the comparison**. What it does not cover is the lane's own window, and the
reason it does not is that until this landing the Vulkan lane had no picture a human ever looked
at: Route D exists to be captured.

The two halves, then:

1. **The lane reproduces the GL world FBO, and there is no composite quad on the Vulkan side.**
   Every world and marker pass writes `gl_Position.y = p.y/uGame.y*2.0 - 1.0` on the engine's
   screen-space y, which grows *downward*, so clip +1 is the bottom of the game frame. GL's own
   composite turns the FBO over on the way to the window, which is why the game looks right.
   `tagpu_vk_fx.c`'s `vp.y = h; vp.height = -h` makes the Vulkan image match that FBO — and the
   ported passes draw **straight into the swapchain image**, so nothing ever turns it back. Eight
   files carry that viewport (`fps`, `fx`, `mark`, `feat`, `unit`, `scaffold`, `gui`, `terr`) and
   five carry a scissor rect mirrored to match it (`fx_scissor` and its four copies).
   `tagpu_vk_shadow.c` is the exception and always was: its header says *NO Y FLIP* because the map
   is an offscreen texture sampled by UV, never presented.
2. **`tagpu_abshot.c` turns the GL half's rows over**, because `glReadPixels` hands back the
   bottom row first and a PPM's first row is the top. For **this** framebuffer the bottom row is
   the game frame's TOP, so the reversal produces an upside-down PPM — which is exactly the pair
   §2.28 describes, and it is why the two halves line up.

So every A/B on this plan has compared two upside-down pictures. That is a valid comparison **of
content** — geometry, colour, coverage and ordering were all genuinely checked and those figures
stand — but two things fall outside it: the lane's presented picture, and any rasterisation rule
whose answer depends on which way up the viewport is. The 32 px below are the second of those.
Nothing culled at that landing (`VK_CULL_MODE_NONE` in all thirteen ported pipelines), so the
winding argument the flip is also justified by bought nothing. The unit body and hard-shadow
pipelines cull now (§2.98), on the unflipped y-down viewport this section settles, with the sign
chosen for it.

**Proven, not inferred, in three steps.** An X capture of the real game window shows the labels
upright and the group digit *below* its bar; an X capture of the Route D window shows them mirrored
and the digit *above*. Rebuilding this pass alone with `vp.y = 0; vp.height = +h` put the Route D
window upright — and broke the A/B to **20 540 px with the non-black counts identical at 10 361 a
side**, which is a mirror and nothing else. Undoing the capture's row-reversal on that same pair
gives **0 differing pixels of 786 432**.

That last number is why the flip is filed as its own landing rather than noted: **the 32 px this
landing measures ARE the flip**, and they are the first thing on this plan that the matched
upside-down pair could not absorb. A horizontal line whose window
y is an exact integer floors to one row under GL and to the other under a mirrored viewport, since
`floor(h − y)` and `h − 1 − floor(y)` agree for every y except an integer. **And the order
markers' y lands on one half the time, for a reason worth writing down**: `tagpu_order.c`'s
`project` computes `sy = wz − walt·0.5 − eyeY + 32`, and for an order target `rec_pos` hands it
the 16.16 snapshot of an integer world coordinate — so `sy` is an exact integer when the target's
terrain altitude is **even** and a half-integer when it is **odd**. That is why some crosshairs in
the difference image tie and others do not, which "the engine projects them with integers" alone
does not explain. Vertical strokes are
unaffected (x is not flipped) and area primitives are unaffected (their sample points are at
half-integers and never on a boundary), which is exactly the pattern the difference image shows.

#### The numbers

Fixture `selbox-facings`, 1024×768, `ss=1`, zoom 1, `mark.on=log order.on=log vk.on=color=0,0,0`,
three ARMSTUMPs on a patrol with SHIFT held, one hovered, `ctrl+1` assigned and `+showranges` on.
The captured frame carries bars, route dots, route lines, range circles, nine labels and three
group digits — verified in the captured image itself, not from the `mark:` counter line, which
prints every 120 frames and is therefore the fixture's state and not the frame's.

| fixture / build | non-black GL | non-black Vulkan | differing of 786 432 |
|---|---|---|---|
| bars only — the first fixture | 297 | 297 | **0** |
| six kinds, as first built | 10 208 | 15 097 | 5 144 |
| + Bresenham lines | 10 266 | 14 157 | 4 066 |
| + the text atlas bound | 10 324 | 10 324 | **32** |
| …the same pair, **both** the viewport flip and `tagpu_abshot.c`'s row order corrected | 10 361 | 10 361 | **0** |
| the band box held open (`cursor=8`) | 2 456 | 2 456 | **0** |
| the captured post-fog layer (`nocursor`, `postfog=captured`) | 2 456 | 2 456 | **0** |

**Read that fifth row carefully: it takes TWO changes, not one.** Correcting the
viewport alone leaves the A/B at **20 540 px** — the Vulkan half is then upright and the GL half
is still reversed, which is a mirror and nothing else. 0 px is what the pair measures once
`tagpu_abshot.c` stops turning the GL rows over as well. [Caught by the landing review, which read
the table against the prose above it.]

**And the GL column moves on every row, including rows whose only change was Vulkan-side**, because
the fixture is live — three units on a patrol, a pointer hovering one of them — and is not
frame-reproducible. Each row's own diff is a paired measurement of one frame and is evidence;
differences *between* rows' GL counts are not.

#### Not covered

* **The cursor bucket and the post-fog layer are no longer open — both closed at 0 px**, as
  the last two rows of the table. They take a band box held open across the capture
  (`down:lbutton`, move, and no release until after), and they are mutually exclusive by lever
  because `nocursor` is what fills the layer — so they are two captures, not one. **All seven draw
  kinds are now measured.** Both come out 0 px even with the flip in place, and that is the
  tie-break argument confirmed from the other side: the band box is built by `put_outline` →
  `put_bar` → `put_barf`, which emits triangle quads, and the layer is a textured quad. An area
  primitive's coverage is decided at sample points on the half-integer grid, which never land on a
  pixel boundary, so mirroring cannot move one. Only a zero-width line can tie.
* **One map, one resolution, one GPU, one OS, `ss=1`, zoom 1.** The `ss != 1` line width is
  *refused* rather than drawn, so it is a bound and not a gap; zoom is untested either way.
* **The flip above is diagnosed and proven and NOT fixed** — it is nine files and every pass's A/B
  has to be re-run, which is a landing and not a rider on this one.

### 2.40 The lane's frame was upside down, and no A/B could see it — landing 5b of the Vulkan-only plan

**FOUND BY THE OWNER, LOOKING AT THE ROUTE D WINDOW, 2026-09-17**, while landing 5's labels were on
screen for the first time. The text read mirrored. It was not the text: **every world pass drew the
whole frame upside down**, and had since G19e.

#### Two y conventions, and the flip is right for one of them

This tree's GL shaders do not agree about which way y runs, and that is not sloppiness — they draw
into different targets:

| convention | passes | where the GL twin draws | Vulkan needs |
|---|---|---|---|
| `p.y/uGame.y*2 − 1` on the engine's screen-space y, which grows **downward** | `terr`, `feat`, `fx`, `unit`, `mark` | the **world FBO**, whose clip +1 is the bottom of the game frame; GL's composite quad turns it over on the way to the window | **no flip** — clip −1 is already the game's top row, which is row 0 under Vulkan |
| `1 − y*2`, or an NDC rect built y-up | `gui`'s composite, `fps`, `scaffold` | GL's **window**, in GL's own convention | **the flip** — a negative viewport height, because the two APIs disagree about which row of a window is the top |

The lane applied `tagpu_vk_fx.c`'s negative viewport height to **all** of them. For the first group
that is a second turn, and the ported passes draw **straight into the swapchain image** — there is
no composite quad on the Vulkan side to turn it back. So Route D presented the world upside down.

`tagpu_vk_shadow.c` was never affected and its header already said why: its map is an offscreen
texture sampled by UV, never presented, so it takes a positive height. That was the one pass whose
author asked the question this section is the answer to.

#### Why eight landings of A/Bs could not see it

`tagpu_abshot.c` turned the GL half of every capture over by the same rule, because `glReadPixels`
hands back the bottom row first and a PPM's first row is the top — which is correct for a
default-framebuffer readback and wrong for the world FBO, whose first row IS the game's top row. So
both halves were mirrored and they lined up exactly.

**This was written down and accepted, in as many words.** §2.29: *"Both halves of this A/B are
upside-down pictures of the world, and that is correct… They are upside down identically, which is
the only thing the comparison asks."* Every clause is true **of the comparison**. What none of it
covers is the lane's own presented picture — and until landing 5 the Vulkan lane had no picture a
human ever looked at. Route D exists to be captured.

**The general lesson, and it is the one worth carrying:** an A/B compares the two lanes **to each
other**, so it is structurally blind to any error they share. Every figure on this plan stands as a
*content* comparison — geometry, colour, coverage and ordering were all genuinely compared — but two
classes fall outside it: the lane's presented picture, and any rasterisation rule whose answer
depends on which way up the viewport is. **The screen is the oracle for those, not `vk-ab.py`.**

#### What changed

* **Five viewports**, `vp.y = h; height = −h` → `vp.y = 0; height = +h`.
* **Five scissor rects** that were deliberately mirrored to compensate (`fx_scissor` and its four
  copies): `y0 = h − (ytop + hh)` → `y0 = ytop`. They are the same rectangle on both sides now.
* **`TAGPU_ABSHOT_TOPDOWN`**, set by exactly those five passes' GL halves. It is a flag and not a
  blanket change because the reversal stays right for `gui`, `fps` and `scaffold`. With the flag
  clear, `write_ppm` emits rows `h−1 … 0` — byte-identical to the loop it replaced, so the change
  is a provable no-op for those three.
* **Four `flipok` refusals deleted.** `VK_KHR_maintenance1` was needed *only* for the negative
  height — `tagpu_vk.c` says so itself ("*it buys exactly one thing*"), and nothing in the lane uses
  `vkTrimCommandPool`, a 2D-array view of a 3D image, or `VK_ERROR_OUT_OF_POOL_MEMORY`. So `terr`,
  `feat`, `fx` and `unit` no longer stand down without it; `gui`, `fps` and `scaffold` still require
  it and still ask.
  **AND THAT CHANGES WHAT A DEVICE WITHOUT IT SEES.** Before, no ported pass could flip and none
  drew, so Route D presented the clear colour. Now the five world passes draw and the three overlay
  passes stand down — **a world with no UI over it**. That is not a regression by this project's
  rule (each pass still reproduces its own part or refuses), and it is the same shape as the
  shipped configuration in §2.35, where the lane draws the UI and nothing else. It is written down
  here and in the seam's own log line because it is a behaviour nobody chose deliberately.
* The prose that argued for the flip, in five pass headers, in `tagpu_vk_pass.h`'s `flipok`
  contract, and in four places in this file — including three `NO CULLING` comments whose stated
  reason was the winding the flip reversed. Nothing culled then; the unit body and hard-shadow
  pipelines cull since §2.98, for the engine's reason, not the flip's.

#### How it was verified — and the method is the point

**The screen, not the A/B.** Both windows are captured by **window id** and diffed:

```
DISPLAY=:0 xwininfo -root -tree | grep 1024x768     # the TITLED window is the game;
DISPLAY=:0 import -window <id> out.png              # its untitled sibling at the same +X+Y is Route D's
```

Two traps, each of which cost a run:

* **Two instances park at the same coordinates.** `park.sh` puts every window in the same place, so
  "the untitled sibling at that position" can belong to a *different* instance. Stop all but one.
* **An obscured window's backing store is stale.** Route D covers the game completely, so an
  `import` of the game window returns whatever it last held — in one run, the pre-scenario frame,
  which read as a 534 730-px difference that was really two different moments. Capture the game
  **before** the lane is armed, on a static fixture.

**Each of the three that KEEP the flip was established on the screen separately**, because "its
shader says so" is the inference this landing exists to distrust:

* **`gui`** — with every pass armed, the difference between the two windows was *exactly* the world
  viewport, `(128,32)–(1024,736)`. The side panel, the top bar and the bottom bar were identical
  pixel for pixel, which is the composite reproducing them upright while the world was mirrored.
* **`fps`** — the readout renders `FPS451` in Route D's top-left, upright and legible, at the
  corner the GL window puts it in. A mirrored readout is unmistakable and this is not one.
* **`scaffold`** — the overlay tints its silhouettes by row, blue at the top to red at the bottom.
  Fitting `d(R−B)/dy` over the tinted pixels gives **+0.0166** in the GL window and **+0.2088** in
  Route D's: the same sign, so the same way up. (The GL slope is shallower because the tint is
  composited over terrain there; only the sign is the evidence.)

| measurement | result |
|---|---|
| GL frame vs Route D, every pass armed | **311 px of 786 432** |
| the same pair, Route D mirrored | 624 839 px |
| `terr` A/B | **0 px** of 786 432 (630 708 non-black a side) |
| `feat` A/B | **0 px** (103 638 a side) |
| `unit` (`posedraw`) A/B | **0 px** (2 125 a side) |
| `fx` A/B, `fx-rockets` | **0 px** (1 298 a side) |
| `gui` A/B — **untouched control** | **0 px** of 307 200 (306 937 a side) |
| `mark` A/B, six draw kinds | **0 px** (10 305 a side) — **was 32 px**, and those 32 were this |
| `mark` A/B, the band box | **0 px** (2 456 a side) |
| `mark` A/B, the post-fog layer | **0 px** (2 456 a side) |

**[THE FIGURE ABOVE WAS 1 394 px UNTIL 2026-09-17 AND THE METHOD WAS WRONG.]** The first version of
this comparison captured the game window *before* arming `vk.on`, because Route D covers the game
window completely. **Arming the lane after launch starves it**: the mirrors it samples are asked for
on the 30-frame arm poll, and a lane that comes up mid-session never gets the ones established at
launch. Measured, on `one-unit` with `native.on` alone and nothing else moved: **382** non-black
pixels in Route D with `vk.on` armed at launch — the commander, matching that fixture's A/B exactly
— against **89** with it armed late, where the unit's body is simply absent. That artefact read as
"the Vulkan lane draws a commander black", and it is not a rendering fault at all. The corrected
method reads the GL side with `tacli glshot`, which goes to the GL lane rather than to an obscured
window, and leaves `vk.on` armed from launch.

**The 311 px are described rather than fully attributed**, which the 1 394 were not. They sit in
four 64-px cells — x 516..732, y 363..398 — on two of the fixture's three parked ARMSTUMPs, at a
worst channel of **75**: a shading difference on two units, not a missing or displaced object.
**Nothing outside the world viewport differs at all**, so the GUI layer is pixel-identical. Two
plausible contributors and neither is separated here: this run used the **play** anisotropy default
rather than the `aniso=1` substitution §2.37 established for unit comparisons, and the two captures
are still not the same instant. The same-frame check on this fixture is the unit pass's own A/B,
which reads **0 px**.

#### What it closed

**Landing 5's 32 px were this, and the prediction was made before the measurement.** A horizontal
line whose window y is an exact integer floors to one row under GL and the other under a mirrored
viewport, because `floor(h − y)` and `h − 1 − floor(y)` agree for every y but an integer. With the
flip gone, the same fixture with the same six draw kinds measures **0 px** — and the two marker
fixtures that were already 0 with the flip in place (the band box, the captured layer) are still 0,
which is the other half of the argument: both are AREA primitives, whose coverage is decided at
sample points on the half-integer grid, and mirroring cannot move one of those.

#### Two fixture defects found on the way, neither caused by this landing

* **Gate 2's feature A/B recipe has been unusable since gate 3a.** `feat` needs `native.on` armed to
  own its leaf, and gate 3a is what made the Vulkan **unit** pass draw — so the seam now refuses
  with *"1 A/B levers claimed this frame and 2 passes drew into it"*. Gate 2 could take that
  measurement only because the unit pass was still standing down on the atlas mirror. The workaround
  is to pan the camera off every unit (`tacli eye`) until `native: 0 unit(s)`, which leaves `feat`
  the only Vulkan pass drawing.
* **`fx-lasers` does not reliably fire.** Two runs measured `proj=0 laser=0` at the capture frame, so
  the GL half never reached the disk and the A/B produced nothing. `fx-rockets` holds model
  projectiles in flight for minutes and is the fixture to use. This is the "a 0 px from a fixture
  that never took the branch" trap in its better form: no result rather than a false pass.

#### And one premise it turned over on the way

`tagpu_vk_unit.c` stood the unit pass down on any frame whose twin had the scaffold overlay armed,
because `TAGPU_GLSL_SCAF_TEST` reads `gl_FragCoord` and *"this pass's flipped viewport makes the two
exact mirrors"*. **That reason is now inverted**: `tagpu_glsl.h` states that the GL VS maps game row
0 to FBO window y 0, so GL reads `g + 0.5` for game row g, and with a positive height this lane
stores game row g at image row g and reads `g + 0.5` too. The two agree. The paragraph ended *"any
later pass whose fragment shader reads `gl_FragCoord` inherits this"*, so it is corrected in place
rather than deleted. [Found by the landing's review.]

**The refusal itself stays, on the half that is still true**: `tagpu_vk_scaffold_view` returns
VK_NULL_HANDLE for a frame or slot that is not its own and `bind_main` then binds a 1×1 stand-in,
which against a twin sampling a real overlay is a different picture. Gating the refusal on the view
rather than on `scafOn` is now a small bounded change — but it enables a path this lane has never
measured, and it is left for the landing that measures it.

#### Not covered

* **The `fx` figure above is model projectiles and sprites, not lines** (`lines=0` in that frame).
  §2.31's 3–6 px tie-break residual was measured on a laser **bolt**, and the argument there is the
  same mirroring this landing removed — so it should be closed too, but that is **reasoned, not
  measured**, and it needs a laser fixture that actually fires.
* **One map, one resolution, one GPU, one OS, `ss=1`, zoom 1.**

### 2.41 The build ghost, carried at last — landing 6 of the Vulkan-only plan

**MEASURED BEFORE IT WAS STARTED, AND THE COST WAS TOTAL.** The plan's row read *"not reached today
— the unit pass refuses on the atlas mirror several checks earlier — so its cost is still
unknown"*. Gates 3a and 3b removed that earlier refusal, so it is reached now, and the first thing
this landing did was find out what it costs. With one ARMCOM selected and a solar placement open
(cursor mode `0x0E`, confirmed by peeking `main+0x2CC3`):

```
ghost: curs=6193 queue=0 drawn=6193 nobake=0 trunc=0 alpha=0.40
vk: unit: the GL twin drew 1 posed unit(s) this hand-over does not carry
          (a build ghost, or past its cap) - nothing drawn while that is true
```

**For as long as a building placement was open the Vulkan unit pass drew nothing at all** — not the
ghost, and not the units either. `tagpu_ghost.on` is a play default gated on `tagpu_native.on`
(`tagpu_opt.c`), and placing buildings is most of what a TA player does, so this was the ordinary
case and not a corner. That is gate 3a's lesson applied rather than quoted: measure the refusal
before porting the stream behind it.

#### It is a change to the publish window, not a removed exclusion

The obvious reading of `pd_record` — `if (!s_recording || u->ghost) { s_other++; return; }` — is
that ghosts are excluded by that one clause. Dropping it changed **nothing**, and the verification
still read `otherDraws = 1`. `ghost_pass` opens a **second** `tagpu_posedraw_begin`/`_end` window,
and `s_recording = (s_win++ == 0)` meant only the **first** window of a frame recorded, so a ghost
never reached that clause at all.

Every window records now. The first is still the one that publishes the view and the one the A/B
brackets — those are separate questions from which windows record, and conflating them is what hid
this for as long as it was hidden.

#### The ghost is the same draw, which is why the consumer is one pipeline

A ghost rides `tagpu_posedraw_unit` with the same uniforms as a unit. It differs in exactly two
things: `alpha` (0.40, which `pd_record` already carried) and that the GL twin brackets its ghosts
in `glDepthMask(GL_FALSE)` — *"the ghosts blend with each other"*, while units drawn earlier still
occlude them, because the depth TEST stays on. So `tagpu_vk_unit.c` gains `s_pipeGhost`: the body
pipeline with `depthWriteEnable = VK_FALSE` and nothing else moved, bound per draw kind. The
records arrive units-then-ghosts because `pd_record` appends as each draw is issued, so in practice
it switches once.

#### A caster the GL depth pass never drew, caught before it shipped

`ghost_one` builds its record with `memset(&q, 0, sizeof q)` and then sets `alpha` and `ghost`, so
**`castSkip` stayed 0**. That was harmless for exactly as long as ghosts were refused outright —
but `pd_record` computes `casts = (s_depthOn && !u->castSkip)`, so the moment ghosts are carried
every one of them is handed over as a **caster**. That count is what `tagpu_vk_unit_casters` feeds
to the shadow census, which is compared against the GL side's own `otherCasters`; an extra caster
there is a **wrong shadow map, not a missing one**. `ghost_one` now sets `castSkip = 1`, which is
simply true: the depth loop runs earlier in the frame and over the real units only.

#### The review found two defects in the above, and both were real

**THE GHOST COMPOSITED ON THE WRONG SIDE OF THE EFFECTS.** `tagpu_vk.c` records terrain, features,
**units**, effects, markers — and carries a comment at that very line saying *two passes that blend
are not commutative*, which is why the body stage sits where it does. `tagpu_native.c` draws the
units, then `tagpu_fx_render`, and **then** `ghost_pass`. Recording the ghosts inside the body stage
therefore put them BEFORE the effects on this lane and after them on the GL one. Both pipelines
blend premultiplied `over`: GL yields `G over (F over D)` and Vulkan `F over (G over D)`, which
differ by up to the ghost's own alpha share of the effect — 0.40·F per pixel — for any translucent
effect overlapping a ghost, which is nano spray on a queued site or an explosion under the placement
cursor. The fixture below has one commander and nothing firing, so the measurement could not see it.

Fixed as a **second record stage**, `tagpu_vk_unit_record_ghosts`, called immediately after
`tagpu_vk_fx_record`. Standing the pass down whenever an effects pass and a ghost coexist was the
alternative and it defeats the landing: effects are armed in all ordinary play.

**AND THE A/B CLAIM WAS DROPPED ON FRAMES THAT DESERVED IT.** The first version read
`s_pub.ab = s_nghost ? 0 : s_abFrame;` followed by `s_abFrame = 0;`, in a publish block that runs at
**every** window's `_end`. The second window re-ran it with the value already consumed, so the pair
went unclaimed **whether or not a ghost had been recorded** — and `s_abDone` had latched, so the
one-shot never retried and the instrument read as a port failure for the rest of the session. It is
reachable on a frame whose queued build sites are all off screen, because `ghost_pass` opens its
window on the site COUNT and culls each site afterwards. A second path was worse: with no posed unit
on screen `tagpu_native.c` skips the unit window entirely, so the **ghost's window is the first**,
and the capture was bracketed around the ghost pass itself.

Both halves are fixed: the bracket opens only on a **non-ghost** window (`tagpu_posedraw_begin_ghost`
is a separate entry point, because counting windows is exactly the test that fails), only the window
that opened it closes it, and the claim is frame-scoped and idempotent.

#### The A/B cannot be this landing's oracle, for two independent reasons

1. **The ghost needs the marker pass.** It arms on `tagpu_mark_cursor_ours()`, so it draws only
   when `mark.on` owns the cursor layer — and the marker pass then draws the placement square.
   Two Vulkan passes in one frame, which the capture refuses outright, and there is no way to keep
   the ghost while removing the square. (`markown` installs its detours at DLL attach, so a
   `mark.on` written to a running instance opens nothing; `ghost: curs=0 drawn=0` with the cursor
   mode already 14 is that signature, and it cost two probe runs.)
2. **The GL half cannot contain a ghost.** It is blacked and read back around the **first** window,
   and the ghost draws in the second — while the Vulkan half is the whole presented image, which
   can. The two halves would differ by the ghost and the difference would be the instrument's.

So the pass **declines to claim the pair** on any frame that recorded a ghost, rather than report
that difference: `s_pub.ab = s_nghost ? 0 : s_abFrame`. Landing 6's oracle is §2.40's two-window
comparison instead, which needs neither a bracket nor a single drawing pass — the second time that
method has paid for itself, and the reason it was worth building.

#### Measured with §2.40's method, and the ghost's own pixels are exact

`one-unit`, 1024x768, `ss=1`, `native.on=all wrecks` + `ghost.on` + `mark.on` + `terr.on` +
`feat.on` + `gui.on`, `vk.on` armed **from launch** (§2.40's trap), the GL side read with
`tacli glshot`, a solar placement held open across both captures.

| | |
|---|---|
| cursor mode | `0x0E`, and `ghost: curs=11998 drawn=11998` |
| `vk: unit: … nothing drawn while that is true` | **0 occurrences** — the stand-down is gone |
| GL frame vs Route D, a ghost in both | **260 px of 786 432** |
| **inside the ghost's own box** (560..650, 380..470) | **0 px** |

**The ghost is reproduced exactly.** The 260 are two 64-px cells elsewhere — x 495..532,
y 449..493, worst channel 184 — on the ARMCOM's own body, which is the same class of residual as
§2.40's 311 px on two parked ARMSTUMPs and has the same two unseparated contributors: this run
used the **play** anisotropy default rather than §2.37's `aniso=1` substitution, and the two
captures are not the same instant.

#### Not covered

* **The queued-build ghosts are not exercised.** `ghost: queue=0` in every run here: the fixture
  opens a placement but never commits one, so only the cursor ghost drew. The queue path goes
  through the same `ghost_one` and the same record, so it is carried by construction, but that is
  an argument and not a measurement.
* **AN EFFECT OVERLAPPING A GHOST IS NOT MEASURED.** The composite-order fix above is by
  construction — the ghosts are recorded in a stage after `tagpu_vk_fx_record`, which is where the
  GL twin draws them — and the fixture here has nothing firing, so no measurement here distinguishes
  the fixed order from the broken one. A fixture that puts nano spray or an explosion under a
  placement cursor would, and none exists.
* **The A/B paths repaired above are reasoned, not re-run.** The frames that exposed them — all
  queued sites off screen, and no posed unit on screen with a placement open — are not in any
  fixture either.
* **One map, one resolution, one GPU, `ss=1`, zoom 1, one building type.**

### 2.42 The restorer comes apart along the line that is not the API — landing 7 of the Vulkan-only plan

**The split was mandatory and the forcing fact is in the tree, not in the plan.**
`tagpu_restoreglsl.c` was one 1 132-line file doing two unrelated jobs: an incremental background
**scheduler** — job queues, batch formation, a per-slice GPU-time budget driven by a smoothed
cost-per-unit estimate — and a **GL draw sequence**. Landing 11 deletes `opengl_utils.h`, which the
file includes. But `tagpu_rglsl_tileable` is called from `tagpu_terr.c:1034` and
`tagpu_gaf.c:1130`, both *gather* halves that survive the deletion, and the job queues are what
every consumer's lazy-restore contract is written against. So the module could neither be deleted
with GL nor left as it was.

**It was done now because landing 4 removes the oracle.** After Route D goes there is no way to
show that a refactor of the GL restorer changed no pixel, and a scheduler refactor whose
correctness is never demonstrated is exactly the kind of silent wrongness this stack produces.

| file | what it owns |
|---|---|
| `tagpu_restore_core.{h,c}` | the weight files (embedded since 2026-10-25: `res.rc` `RMODEL_FULL` / `RMODEL_TINY`, zstd; [compute-restorer](../compute-restorer.html), *A model per job*), the options, the size-class ladder, `tileable`, the job table and its queues, batch formation, the **pass sequencer**, the cost model, the budget arithmetic, every counter, and every log line but two (below). Names no rendering API. |
| `tagpu_restoreglsl.c` | device resources and the three draws, behind a twelve-entry backend interface. |

**The backend is TOLD which draw to make rather than working it out.** `issue_draw` in the core
decides fill / conv / out, advances `pass`, `group` and the ping-pong side, builds the per-slot
tables and the out-pass vertices, and computes the draw's cost in work units; the backend binds and
draws. That is the whole reason the line is drawn there: a second backend that re-derived the
sequence could disagree with the first about which layer it was on, and **no A/B would show it** —
both lanes would be internally consistent and produce different pictures for a reason neither
reports.

**Two things were kept deliberately rather than tidied, and both would have been quiet regressions.**

* **One scheduler per backend, not one shared.** A shared budget is defensible on its merits — the
  total restore work per frame is what matters to the frame — and it would also have changed the GL
  lane's slicing the moment a second lane came up. This split is meant to be pure code motion, so
  each backend declares its own `TAGPU_RSCHED`.
* **The options stay per-CONTEXT.** They were read inside `init_gl`, so a `tiny` or `budget=`
  edited between two GL contexts took effect, and the `ta-drive` skill documents exactly that
  (*"read once per GL context… the startup GL reset re-reads them once, a map change does not"*).
  `tagpu_rcore_reload` is called from a backend's own init to preserve it. Making the model and
  options process-wide is the natural-looking mistake and it would have silently broken a
  documented knob.

**Two diagnostics were reworded, and that is the one place "pure code motion" is not literally
true.** The core cannot say *"timer query"* or *"MAX_UNIFORM_BLOCK_SIZE"* — both are GL terms, and a
file that names no API must not assert them — so those two lines now read *"the GPU timer never
completed"* and *"uniform block %d < one k-block"*. Neither string is referenced by any note, skill
or tool, and **every line that IS documented is byte-identical**, because the lane name the core
prefixes with is `"restoreglsl"`: `lazy restore armed`, `job started`, `done:`, `queue drained`,
`idle: activations freed` and the `%dx%d %s, NK=%d …` banner all keep their exact text.
[The overclaim was *"every counter and every log line"*, caught by this landing's review.]

**MEASURED: the GL restorer's output is byte-for-byte identical.** `tagpu_restoredump.on` writes
the finished terrain atlas with `glGetTexImage`, so the comparison needs no window and no capture.
Same instance, same map, the two DLLs swapped under `--keep-dll`:

| fixture | dump | pre-split | post-split |
|---|---|---|---|
| `static-terrain` | `tagpu_restore.rgba` | 46 461 952 B (2176x5338, 10 036 tiles) | **identical, `cmp` clean** |
| | the `done` line | 10 036 frames (4 142 wrap-padded), 158 batches, 7 426 draws | **the same four counts** |
| `feat-forest` | `tagpu_restore.rgba` | 23 674 880 B (2176x2720, 5 062 tiles) | **identical** |
| | `tagpu_restore_unit.rgba` | 16 777 216 B | **identical** |
| | `tagpu_restore_unit.r8` | 4 194 304 B | **identical** |
| | `tagpu_restore_unit.idx` | 427 B, 25 entries | **identical** |
| | the `done` line | 5 062 frames (400 wrap-padded), 80 batches, 3 760 draws | **the same four counts** |
| | `unit: queue drained` | 25 frames in 2 batches, 94 draws | **the same three counts** |

**EVERY FIGURE ABOVE WAS RE-TAKEN AFTER THE REVIEW'S FIX AND AGREES WITH THE FIRST RUN** — all
five dumps identical again, both `done` lines' counts unchanged, the `queue drained` tally
unchanged. The guard added for the review's one real finding cannot fire on the GL lane, which is
an argument; this is the measurement, and the number now describes the code that landed rather than
the code the reviewer read.

**Two fixtures on two different maps, and the second one is the one that matters most**: the
terrain's job is a fixed list added once, but a GAF atlas's is an **open queue** fed on every miss,
and that is where `job_add` while a run is live, the size-class mixing, the batch-boundary re-pick
and the `queue drained` tally all live. The unit atlas's twin is a product of that path and it came
back byte-identical, index file included.

The wall time and fps differ between the runs (10 337 ms at 122.9 fps against 11 083 ms at
122.2 fps on the first fixture; 4 835 against 4 776 ms on the second) and **that is not a finding**
— the slice budget is wall-clock-driven, so the number of slices a restore takes, and therefore how
many *frames* it is spread over, is a property of the machine that hour rather than of the code.
The counts that *are* code-determined — frames, wrap-padded, batches, draws — match on both
fixtures, and every byte of every atlas matches.

**The idle path is on the record too**: `restoreglsl: idle: activations freed` appears in both
runs, so the 180-slice release fires through the new `act_free` return-value contract.

**THE REVIEW FOUND ONE REAL DEFECT AND IT WAS IN THE NEW INTERFACE, NOT IN THE MOVED CODE** —
which is the right place for it to be, and the reason a refactor gets reviewed at all.
`tagpu_restore_core.h` promised `tagpu_rcore_job_new` returns NULL *"when the table is full **or the
model is unusable**"*, and the code never looked at the model; `tagpu_rcore_ready()`, added in the
same commit for apparently that purpose, was called from nowhere. Without the guard a backend that
creates a job before loading weights gets a live job with `depth == 0`, and the sequencer then walks
off the front of the model: FILL sets `pass = 1`, `pass <= depth` is `1 <= 0`, so the next draw
takes the **OUT** branch and evaluates `layer[depth - 1]` — `layer[-1]`, the four ints in front of
the array, read as an input-tile count.

**The GL lane cannot reach it** (`job_new_x` runs `init_gl` first, which fails on a bad weight file),
so it is zero risk today and a **trap laid for the second backend — which is the entire reason the
interface exists**. Fixed by making the header true. Two dead additions went with it (`s_modelTried`,
written and never read; `TAGPU_RDRAWREQ.cols`, set and never read) and the draw request now states
the tables' actual lifetime: rebuilt on each batch's FILL, same contents for that batch's CONV and
OUT.

The reviewer separately confirmed, against the pre-split file, that the sequencer advances on the
same side of the draw in the same order in all three branches; that the conv cost really does use
the full `NK` rather than the clamped tail; that `pick_job`'s inflight-first loop means the slot
tables can never describe another job's batch; that `state_push`/`state_pop` are balanced across
every early return; that `s_sched.timer` and `s_query[0]` cannot disagree across `glreset`, a failed
re-init or the give-up path; and that a `draw` returning 0 cannot spin.

**What this measurement does NOT cover**, stated rather than implied:

* the **failure** paths — `act_ensure` returning 0 (no float render target), a destination that is
  not a complete render target, the out-of-memory returns. None is reachable on a working device
  and none is exercised.
* the **timer give-up** path: 300 slices without a query result, which needs a driver that accepts
  a timer query and never completes one.
* `job_clear` and `repalette` — the palette-moved-under-a-live-job path. `tagpu_gaf.c` reaches
  both on a palette change, which neither fixture produces.
* **the feature and effects queues were never FED, which is not the same as never drained.** Both
  logged `lazy restore armed` on `feat-forest` and then produced **zero batch lines** with
  `restoreglsl.on=log` on — so `tagpu_rcore_job_add` was never called for either and the `feat` and
  `fx` jobs are covered only as far as `job_new`. The reason is the fixture: those two atlases hold
  **GAF sprites**, and `feat-forest`'s trees are 3DO features, so what they populated was the
  *unit* atlas — which is the one that did get verified. Closing this needs a fixture with real
  2D sprite art in view (`fx-rockets` for the effects side). They run the same queue and batch
  path as the unit atlas, which is an argument and not a measurement.
* two GPUs' worth of nothing: one GPU, one model, NK=4, fp32.

### 2.43 The restorer restores on Vulkan, and two batches in one slice cost six cells — landing 7c of the Vulkan-only plan

§2.42 split the restorer along the line that is not the API and left `tagpu_vk_restore.c` complete,
warning-free and **with no consumer**, which is to say dead. This is the consumer, and it is the
terrain's tile atlas.

**What crosses the hand-over is the WORK, not the picture.** Under `tagpu_restorevk.on`
`tagpu_terr.c` stops reading its own restored twin back for the Vulkan lane — the CPU mirror gate 2
built — and publishes the **frame list** instead: the very array it built for its own GL job, in the
order it built it (`TAGPU_TERRHAND::restoreFrames/restoreN/restoreSerial/restoreRepaint`). The
Vulkan lane then paints its own restored atlas.

The list rather than a "restore it" flag, because three of a frame's eleven numbers are
engine-memory facts and belong on the gather side of the split: `wrap` is
`tagpu_rglsl_tileable()` over the tile's own texels against the **art** palette, and the order is
the centre-out rank over the live tile map (`restore_order`, `tagpu_terr.c`). What crosses is their
result. That is also what makes the two lanes comparable byte-for-byte rather than merely
both-plausible: both restore **the same rectangles in the same order from the same atlas with the
same palette, in the same process, on the same frames**.

**The reach into files this landing does not own is one usage flag.** `mk_image` and
`shared_resize` in `tagpu_vk_terr.c` now take the caller's `VkImageUsageFlags`, because the restored
atlas is rendered into and a colour attachment must say so at create time; the other four images
there are only ever sampled and say so. `COLOR_ATTACHMENT` is unconditional on the restored atlas
rather than lever-dependent — RGBA8 optimal-tiling colour-attachment support is required of every
Vulkan device, and paying for it always keeps the image's identity independent of which path filled
it.

#### The measurement, and the bug it found

**Both lanes restore in one run and the dumps are one `cmp` apart.** `tagpu_terr.c` writes
`tagpu_restore.rgba` off its restored **texture** with `glGetTexImage`; `tagpu_vk_terr.c` now writes
`tagpu_restore_vk.rgba` off the image the Vulkan restorer painted. The Vulkan half does not stall
the device to get the bytes: the copy is recorded into the frame's command buffer and read at **that
slot's next `prepare`**, the one instant the seam's fence has proved the submit carrying it
completed — the argument `shared_slot_done` in the same file already makes for the retire.

`static-terrain`, `full` model, NK=4, fp32, both dumps **46 461 952 bytes** (2176 × 5338 RGBA):

| | GL lane | Vulkan lane |
|---|---|---|
| frames | 10 036 (4 142 wrap-padded) | **10 036 (4 142 wrap-padded)** |
| batches | 158 | **158** |
| draws | 7 426 | **7 426** |
| slices | 853 of 5 319 frames | 918 of 1 276 frames |
| GPU measured | 11 373 ms over 100 % of the work | 12 602 ms over 100 % of the work |

Every code-determined count is identical. **And 6 936 texels of 11 615 488 — 0.0597 % — differed**,
every one of them transparent black on the Vulkan side and coloured on GL's.

**6 936 is exactly 6 × 34², and 34 is the atlas cell pitch.** Clustered by cell, the difference is
**six whole cells of 10 036, each entirely unpainted, and every other cell byte-identical** — not
noise, not a flip, not a rounding difference. That shape names its own cause.

**THE CAUSE: a slice can issue more than one batch, and the OUT draw's vertices were staged per
FRAME SLOT.** `tagpu_rcore_step`'s loop re-picks at every batch boundary and runs until the
GPU-time budget is spent, so batches per slice is bounded by a **time budget and not by a count**;
the terrain's own log shows **batches 157 and 158 both issued at slice 917**. Both OUT draws wrote
their vertices to `s_vmap + s_slot * sizeof(verts)` — the same address — so batch 158's 36 vertices
landed on top of batch 157's 384 before either draw executed. Batch 157's draw then painted batch
158's six destinations with batch 157's slot content, batch 158's own draw repainted those six
correctly, and **batch 157's leading six cells were never painted at all**. Six cells, silent, in a
picture that is otherwise byte-perfect.

**The GL lane has no such hazard and that is why the port did not inherit one.** A
`glBufferSubData` followed by a draw is ordered by GL itself — the driver renames or copies. A host
write to a mapped Vulkan buffer is ordered by nothing.

**The fix is an ordering, and a bigger arena is explicitly NOT it.** Since batches per slice is a
time budget rather than a count, *any* arena is a number that can be exceeded, and what it buys when
it is exceeded is this same silent failure. `vkCmdUpdateBuffer` records the data **at that point in
the command stream**, so each batch carries its own copy and the ordering is the command buffer's
own; the buffer became one batch's worth (12 288 bytes, well inside the command's 65 536 limit) and
device-local instead of eight host-mapped slot regions. The barrier around it is deliberately **both
ways**: `TRANSFER → VERTEX_INPUT` makes this batch's vertices visible to its own draw, and
`VERTEX_INPUT → TRANSFER` makes the previous batch's read of those bytes a fact before this write
lands on them.

**Re-measured after the fix, and again after the review's four fixes** (three runs in all, the last
on the landing as it stands): **`cmp` CLEAN — both dumps 46 461 952 bytes, byte for byte
identical**,
and the counts still 10 036 frames / 4 142 wrap-padded / 158 batches / 7 426 draws on both lanes.
**The collision condition was exercised again in that run** — batches 157 and 158 both issued at
slice 914 — so this is the fix holding under the case that broke it, not the case failing to occur.
That also settles §2.42's recorded-but-untrusted **no-flip derivation empirically**: a flip would
show as every cell's rows reversed, and 46 MB agree.

#### Two more ordering holes, found by the act of wiring a consumer rather than by a reviewer

Both were invisible while the pass had no consumer, and both are the class this stack fails at
silently.

1. **None of the restorer's render passes declared a subpass dependency.** The implicit dependency
   Vulkan adds at `dependencyCount == 0` has `dstStageMask = BOTTOM_OF_PIPE` and
   `dstAccessMask = 0`: it orders the attachment's **layout transition** and nothing else. So FILL's
   writes were unordered against the first CONV's sample of them, every CONV against the next, and
   OUT's write of the consumer's atlas against the consumer's own sample of it one render pass
   later. Drivers that flush at a render-pass boundary hide it, which is the problem and not the
   fix. Each pass now states both ends, and because a `0 → EXTERNAL` dependency's destination scope
   is **every subsequent command**, the two make the chain transitive across slices, submits and the
   consumer's own render pass. `TRANSFER` is in both scopes because the dump's copy is exactly the
   reader a dependency naming only the sampling consumer would have left out. Not `BY_REGION`: a
   conv draw reads a 3×3 neighbourhood and OUT reads a different image entirely, so neither read is
   framebuffer-local. The dependencies are what the spec requires, but not every driver honours
   them. AMD's Windows driver drops them for these passes, so `end_pass` records the same ordering
   again as a pipeline barrier (§2.95).
2. **`dst_ready` transitioned the destination from `UNDEFINED` unconditionally, including on a
   repaint** — which licenses the driver to discard every texel of the atlas the repaint exists to
   recolour *in place*. It would have blanked exactly the world it was added to avoid blanking. The
   job now carries `dstHas` and picks `SHADER_READ_ONLY_OPTIMAL` when there is something to keep.

#### What the landing review found, and it was all lifetimes

Four findings, every one verified against the code before acting and every one real. Three are the
same omission in different places: **the retire discipline was applied to the activations and to
nothing else.**

1. **`job_free` destroyed a live job's device objects outright.** It called
   `vkDestroyFramebuffer` on `dstFb`, destroyed the palette image and view, and
   `vkFreeDescriptorSets` on all five sets — with no fence and no retire. Three of its four callers
   are in a consumer's `prepare`, where the seam has waited on **this slot's fence and no other**,
   so the remaining `slots − 1` submits are still queued naming exactly those objects: the OUT
   render pass named `dstFb`, its set was `setOut[i]`, FILL sampled `palView`. A map change, a GL
   reset or a repaint within `slots − 1` frames of a draw is the whole trigger
   (VUID-vkDestroyFramebuffer-framebuffer-00892, VUID-vkFreeDescriptorSets-pDescriptorSets-00309) —
   a crash on a strict driver, corrupt paint on a lax one. The activations got a whole `RETIRE` for
   this hazard; the job's own resources got none, which is what made it easy to miss. They have one
   now, **sized at the worst case its callers can produce** (`TAGPU_R_MAXJOBS × TAGPU_VK_SLOTS`
   entries, under 4 KB) so the full case is unreachable rather than unlikely — and still handled,
   by draining the device, because a stall is honest and an unlicensed destroy is not.

   **Which trigger is actually reachable today is worth being exact about, and it is narrower than
   the finding says.** A *map change* is not one: §2.16's own measurement is that an in-process map
   change brings the whole Vulkan lane down and back up, so `tagpu_vk_terr_down` frees the job on
   the drained teardown path before any new `prepare` runs. The reachable trigger is the
   **repaint** — `glsl_begin(ta, 1)` on a palette move bumps the serial with the lane still up and
   the job still live — and that path is itself unexercised (below). So this was a real
   use-after-free whose only live trigger is one no fixture produces. **That is not a reason to
   have left it**: the teardown-first ordering is a coincidence of two modules' behaviour, which is
   precisely the "it works because of when things happen" argument `CLAUDE.md` refuses. The fix is
   a lifetime and does not care which path frees the job.
2. **Neither retire was in the teardown.** `down()` destroyed the live activations and flushed the
   framebuffer cache but never touched `s_ret`, so a retire outstanding at teardown — an activation
   grow, or the idle release after 180 quiet slices — leaked two array images, their memory, every
   layer view and up to 32 framebuffers, around **100 MB**, and `vkDestroyDevice` then ran with all
   of it alive. The resize path was accidentally fine (the device survives it and a later
   `retire_slot_done` cleaned up); `vk_down` had nothing that ever would. `lost()` now forgets both
   retires, because a `pending` left standing would have a rebuilt device destroy dead handles.
3. **A job captures `s_atlas.view` at creation and never re-reads it**, so any resize of the
   indexed atlas left its descriptors naming `s_atlas.oldView`, which `shared_slot_done` destroys
   `slots` frames later while the restorer draws on. `restore_want` normally frees the job in the
   same frame, but it sits *below* the resize and every `return 0` and `goto refuse` between the two
   skips it. The drop is now keyed on the view itself, immediately after the resize, where nothing
   can return first. (Reported as PLAUSIBLE; confirmed by reading the early returns.)
4. **The "mutually exclusive by construction" claim above was false**, and this section said it. The
   lever is *polled* until it latches, so it can be created mid-session — and then
   `rgb_mirror_step`'s early return stops updating the mirror while leaving `rows > 0` standing, so
   the publisher ran **both** blocks. A consumer resized its restored image twice in one call and
   uploaded a frozen mirror into the image the other lane renders into; where the mirror's rows and
   the atlas's height disagreed it refused every frame instead and **terrain stopped drawing
   altogether**. Fixed in two places on purpose: the latch frees the mirror (the memory is what the
   lever exists to retire) and the publish is an either/or (so the hand-over's own claim does not
   depend on a lever's latch order). Found independently by this session the same hour, which is
   worth recording only because it is the one finding the review did not have to catch.

**What the review verified clean** and is therefore worth not re-deriving: no SPIR-V word changed in
the eleven pre-existing headers (only the `transform` hash and the new comment lines); `rp_deps`
covers the whole chain including the dump's copy in both directions; `dst_ready`'s `dstHas`/
`clearDue` layouts are right on first job, repaint, `job_clear` and post-`down`; the weight offset
is a multiple of 256 given `offset & 15` and `kstride & 15`; `form_batch`'s two passes use an
identical predicate; `fb_for`, `s_rpAct[]`, `s_pipeConv[]`, `conv_spv` and the class indices are in
bounds; and `tagpu_restoreglsl.h` has no includes and no GL types, so `tagpu_terr.h` including it
drags nothing into a `_vk` translation unit.

**`tagpu_vk_restore_lost` still has no caller** outside `down` — the Vulkan seam has no
device-loss path, unlike the GL lane. It is documented, dead, and kept deliberately; the day the
seam grows one it is what that path needs.

#### Where the slice sits in the frame, and what it is not counted in

`tagpu_vk_restore_step` is called **last of the seam's `prepare`**, after every consumer. That is
the reverse of the shadow map's ordering and the same argument: the shadow map must run *before* the
passes that sample it, and the restorer must run *after* the passes that **feed** it, because a
consumer hands it a job and a frame list in its own `prepare`. Its own render passes are begun
inside `prepare`, which is legal there and nowhere else because render passes may not nest.

**It is deliberately not added to `ndraw` or `nclaim`**, for the reason the shadow pass is not:
those count the passes that put pixels in *this* frame, and counting one that puts none would refuse
every A/B capture taken with Classic++ on — which is exactly the configuration restored art is
measured in.

#### What this does NOT cover, stated rather than implied

* **One consumer of four.** The features, effects and unit atlases still take the CPU mirror. Their
  wiring is the same shape (one usage flag plus a published frame list) and is not done.
* **`repaint` is built and not exercised, and it takes two fixes down with it.** The
  palette-moved-under-a-live-job path needs a fixture that moves the palette; `static-terrain` does
  not, and an in-process map change tears the lane down instead of repainting. So `dstHas` is
  argued from the spec's discard rule rather than measured, **and the job retire's licence is
  unexercised for the same reason** — every path that frees a job while submits are in flight is a
  repaint. Both are lifetimes rather than timings, which is what makes them defensible unmeasured;
  neither is *shown*.

  **AND THE FIXTURE WAS ATTEMPTED, TWICE, AND IS RECORDED AS A NEGATIVE RESULT** (2026-09-17)
  rather than left as a to-do, because what it found bounds the gap. Three mechanisms were tried
  against a settled `static-terrain` restore, each confirmed by a **peek** rather than by
  inference:

  | mechanism | what it should move | what happened |
  |---|---|---|
  | the in-game options screen (`tab`, then `escape`) | the presented palette | no change — the palette serial stayed 2 |
  | `+showranges`, a typed cheat the `ta-drive` note records as working, used here as the **control** | `main+0x391BF` | **stayed 0** |
  | `+gamma 13` (`0x4172B2`, `SetGamma(N × 0.1)`) | `main+0x37F08`, then the palette | **stayed 12**, through both `+gamma13` and `+gamma 13` |

  The control failing is what makes this conclusive: it is **not** `+gamma` being refused, it is
  the chat path. Delivery is not the problem either — `tagpu.log` carries
  `input: keys x char:+ char:s …` for every token, and `tagpu_shield.c:362` translates
  `WM_TAGPU_CHAR` into a real `WM_CHAR` for the engine. The likeliest cause, **unproven and marked
  as such**: the Enter that opens the chat line has to reach the engine's own key handling, and TA
  takes game keys through DirectInput rather than from the window, so a posted `WM_KEYDOWN` never
  opens the line and the `WM_CHAR`s that follow have nowhere to land.

  **What this does and does not license.** It does *not* mean the repaint is dead code — the engine
  has a gamma slider in its own UI, so a player reaches this path and both fixes have to be right.
  It does establish that **the palette does not move on its own once a level is up**: three runs,
  two fixtures, the serial reaching 2 *before* the tile atlas is even built. So the next attempt
  should stop trying to provoke it through injected chat and either drive the gamma slider as a
  gadget ([gui-gadgets](../gui-gadgets.html) places it on the campaign screen) or reach `SetGamma`
  another way. That is still the next thing worth building for this lane, and it is now a smaller
  and better-aimed question than it was.

* ~~**The `restorevk` lever is measured in one configuration only**~~ — the lever is gone. The
  restorer follows Classic++'s `assets=` knob, which is a play default and which the
  render-options screen's `Undithered assets` row writes, so the mid-session flip is the
  ordinary case and is run in both directions. See *Undithering ships on* below.
* **The `g_alloc` ring is bounded by the core stopping short of it (`vk_room`, G20d).** The ring
  holds 256 blocks per slot per slice. A FILL takes one, a CONV one and an OUT two.
    - **A `full`-model batch at NK 4 is 47 draws:** the FILL, 45 CONVs (four groups on each of
      eleven 64-channel layers, one on the output) and the OUT, so it spends 48 blocks. The
      terrain's own line gives the count: 3 760 draws in 80 batches. So **the sixth batch in one
      slice overflows the ring**.
    - **This bullet used to say 15 blocks a batch and "about seventeen batches"**, counting one
      CONV a layer. It also said the ring was not reached on the reference setup. Both were wrong.
    - **MEASURED 2026-09-24**, `fx-rockets` under `--defaults` at 1024 × 768, once in each of
      two launches, one of `b14e9b4` and one of the code at `4143757`. Both times four small
      feature batches and two effect batches issued in one slice. The job holding the sixth
      failed, and so did every job that drew after it in that slice: the effects and
      the UI in one launch, and the units as well in the other. A failed job latches its lane
      unrestored (`s_rjTried`) until the pass is next torn down. Classic++ therefore drew those
      lanes from the base atlas for the rest of the launch.
    - **Now the core asks the backend's `room` before every draw** and ends the slice on 0. The
      batch in flight resumes on the next slice, as it does when the budget cuts it. `vk_room`
      holds back each live job's mip levels, which `chain_step` reduces from the same ring after
      the slice. A `_Static_assert` keeps 6 jobs × 12 levels + one draw inside the ring, so the
      head of a slice always has room. `g_alloc`'s line stays as the backstop and is
      unreachable while that holds.
    - **MEASURED 2026-09-24**, the same fixture, two launches with the bound. Neither logged a
      ring line or a failed restore, and every lane's queue drained again and again through the
      fight. Batches finishing per slice: 1 to 5 in both launches, and 6 in one slice of each,
      where a batch the previous slice had cut short finished first. In one case five batches
      filled slice 302 and the next was cut short in 303 and finished in 304. Before the bound,
      the sixth batch in a slice failed its job.
    - **The earlier failure was silent**, which the review caught: the draw returned 0 with no
      log line, so the consumer reported "the restore failed" and nothing said why. `g_alloc`
      names the reason once per slice.
* **One GPU, one model, one fixture, one map.** `tiny`, `fp16`, and any other device's limits are
  unmeasured on this lane.
* **The failure paths** — `act_ensure` returning 0, a destination that is not a complete render
  target, the out-of-memory returns — remain unreachable on a working device and unexercised, as
  §2.42 already records.

### 2.44 Two more consumers, and the parameter tables had the vertex bug too — landing 7d of the Vulkan-only plan

Landing 7c left three of the restorer's four consumers on the CPU mirror — four being the list the plan names; the UI atlas is a fifth and is not on it. This one wires the two
sprite atlases — **features** and **effects** — and the oracle it needed found a bug in the code
7c had already landed, in the sibling of the resource 7c fixed.

**What crosses the hand-over is different from terrain's, because the queue is different.** The
terrain's restore is a fixed list published whole under one serial: 10 036 tiles, arrived at once,
and a consumer that misses a frame re-reads the whole list on the next one. A GAF atlas is a
**lazy queue** — `tagpu_gaf.c`'s `restore_enqueue` adds one frame per miss for the life of the
atlas — so what is published is an **append-only list with a generation**, and the consumer holds
a **cursor** into it:

| | terrain (landing 7c) | features / effects (7d) |
|---|---|---|
| shape | the whole list, per serial | append-only, per generation |
| consumer state | the serial it built from | the generation **and a cursor** |
| a frame the consumer skipped | re-read next frame | still there; it takes more next frame |
| what restarts it | the serial moved | the generation moved |
| bound | the map's tile count | **four times the atlas's entry ceiling** |

`rlistGen` is the only thing a cursor cannot survive, and it is bumped by every event that makes
the array stop being a continuation: the arm, a recycle, a repack (both of which drop the GL
queue and blank the twin), a GL context loss, a palette move, and the overflow restart. A
consumer that sees a new generation drops its job and starts at index 0. `rlistRepaint` is 1 for
the palette-move generation alone, where the destination keeps what it holds.

**The bound is a restart, not a bigger buffer.** An append-only list fed for a session's length is
not bounded by anything; what *is* bounded is "the entries the atlas holds", so reaching the cap
re-seeds the list from those (a new generation, repaint 0) and the other lane blanks and repaints.
The arm path and the overflow recovery are **the same function** on purpose — it means the rare
path is the one exercised on the first frame of every session.

**Arming it frees the read-back's 16 MB**, and the publish is an either/or rather than a claim of
exclusivity — the lever is a poll that can land on any frame, which is exactly what made the same
claim wrong on landing 7c. `tagpu_gaf_atlas_mirror_rgb` also refuses while the list is armed, so
the two can never both be live.

**The restored twin's image now carries `COLOR_ATTACHMENT`** (`mk_image` takes the usage it is
for, in both passes), and **the restored refusal cannot always be a `return`.** This lane's own
restore needs the frame's indexed atlas uploaded before it can paint anything, and that upload is
below the refusal: returning on the frames before the first paint is a **deadlock**, not a
stand-down — nothing drawn because nothing painted, nothing painted because the atlas never
arrived. Those frames now run the uploads and stop without claiming the frame.

#### The bug: the parameter tables were staged per FRAME SLOT

`upload_tables` wrote each batch's three per-frame tables — destination rect, source rect, **key
and wrap** — into a host-mapped region indexed by the frame slot, under a comment that said *"one
batch is ever in flight, so nothing else writes them in between"*. That is true of the **device**
and false of the **recording**: a slice can issue more than one batch, which is landing 7c's own
finding, and both batches' `memcpy` landed on the same address before either
`vkCmdCopyBufferToImage` had executed. **The first batch's frames were therefore restored through
the second batch's tables** — wrong source rects, and wrong colour keys.

**The images themselves were already correctly ordered**: the write-after-read barrier in front of
each batch's copy names the previous batch's shader reads. That is exactly why the bug survived
7c — reasoning about the *images* finds nothing wrong, and the hole is in the staging buffer.

**What it looked like, which is why a sprite atlas was needed to see it at all.** A frame whose
key came from another frame's table has no keyed texel where it should have one, so the OUT pass
writes the key's own palette colour — opaque **(84, 84, 252)** — where the GL twin writes
`(0, 0, 0, 0)`. Measured on the first run of the sprite oracle, `fx-mix` at 1024×768:

| atlas | frames | entries differing | texels differing | the difference |
|---|---|---|---|---|
| `terr` | 5 062 tiles | — | **0** | identical |
| `feat` | 915 painted | **118 of 1 304** | 68 411 of 4 194 304 (1.63 %) | 13 803 of them opaque key colour against `(0,0,0,0)`; the rest wrong source rects |
| `fx` | 139 painted | **3 of 167** | 2 891 (0.07 %) | colour deltas to 112, alpha equal |

Every differing texel lay inside an entry's own cell, and 1 186 of 1 304 feature frames were
byte-identical — the signature of a per-batch parameter swap rather than of a wrong pass.

**The terrain could not have revealed this.** 10 036 tiles of one size with **no colour key at
all**, so a swapped table costs a source rect and nothing else; and the two runs that measured it
gave each slice one batch. It took a consumer whose frames have different sizes *and* a key —
which is the argument for wiring consumers rather than declaring the port done.

The fix is the same shape as 7c's and for the same stated reason: `vkCmdUpdateBuffer` puts each
batch's tables **in the command stream at the point of its own copy**, with a buffer barrier both
ways (TRANSFER → TRANSFER, because the previous batch's read of these bytes is a copy and not a
draw). A bigger arena is still not the fix — batches per slice is a time budget, not a count. The
staging buffer became device-local and unmapped, like the vertex buffer beside it.

**Re-measured, and with the condition present.** `fx-mix` again: all three atlases byte-identical,
**62 slices issued more than one batch** on the two sprite jobs while it ran, which is the
collision exercised rather than avoided. Then `static-terrain`, the fixture landing 7c measured on:
terrain byte-identical at **46 461 952 bytes**, features and effects identical beside it.

| run | fixture | `terr` | `feat` | `fx` |
|---|---|---|---|---|
| 1 | `feat-forest`, wrong arm set | identical | **no atlas at all** | **no atlas at all** |
| 2 | `fx-mix`, before the fix | identical | 118 of 1 304 frames differ | 3 of 167 differ |
| 3 | `fx-mix`, after | identical | **identical** | **identical** |
| 4 | `static-terrain`, after | **identical** (46 MB) | identical | identical |
| 5 | `fx-mix`, after the review's fixes | identical | identical | identical |

**And the terrain's clean result in landing 7c was never in danger, which took the landing review
to see.** The paragraph here first recorded it as an unexplained survival — 7c's re-measurement was
taken with batches 157 and 158 in one slice, so the swap should have cost batch 157 its cells — and
that reading was wrong about what the log line says. **`batch %d … issued at slice %u` is printed
in the OUT branch** (`tagpu_restore_core.c:474`, after `j->rbatches++`), so it timestamps a batch's
LAST draw. The tables are uploaded at the **FILL** (`tagpu_vk_restore.c`, the `TAGPU_RDRAW_FILL`
branch, its only call site), and the budget check sits between individual draws
(`tagpu_restore_core.c:607`), so a batch's FILL is normally several slices before its OUT:

| | what the log line shows | what the resource needs |
|---|---|---|
| the OUT vertices (7c's bug) | two batches' **OUTs** in one slice | exactly that — the vertices are written at OUT |
| the parameter tables (7d's bug) | nothing | two batches' **FILLs** in one slice |

So in 7c's run there was **no table collision at all** — batch 157 was a full 64-frame batch
(`cols` 8, so `TW = TH = 8S` and tens of slices of fragments) while batch 158 had 6 frames
(`cols` 3, about a seventh of the cost, cheap enough for its whole FILL→CONV→OUT chain to fit in
what was left of the budget). Nothing survived the swap; the condition was never present. **And
that is a stronger version of this section's own argument**: the sprite atlases' batches are small
and numerous, so two whole batches — two FILLs — land in one slice routinely (the 62 two-batch
slices measured here), while the terrain's big batches put at most one FILL in a slice. A clean
terrain run says nothing about this hazard either way, and the recipe in
[ta-drive](../ta-drive.html) was telling the next operator to look at the wrong log line.

#### What else this landing found

**The sweep the fix asked for, since this is the second time.** Every other place in
`tagpu_vk_restore.c` that writes device-visible bytes at RECORD time, checked rather than assumed:

| what | when it is written | why it is safe |
|---|---|---|
| the globals ring (`g_alloc`) | once per draw | a **ring**: every allocation in a slice has its own offset, and the slot's fence is what licenses re-use across slices |
| the OUT vertices | once per batch | in the command stream since landing 7c |
| the three parameter tables | once per batch | in the command stream since this landing |
| the palette staging (`upload_pal`) | per job, when `palDue` | per **job**, not per slot, and a second write before the first copy executes leaves the image holding the NEWER palette — a batch can sample a palette one frame early, which is a staleness and not a swap |
| the dump staging | never written by the CPU | a copy DESTINATION, read after the slot's fence |

**And two of the twelve interface entry points have no callers at all** — `job_repalette` and
`job_clear`. Both consumers answer a palette move and a recycle by **rebuilding the job** on the
new serial or generation, which is what makes `repaint` reachable at all, so neither entry point
has ever run. That matters for the next consumer rather than for this one: `job_repalette` sets
`palDue = 2`, whose barrier names `SHADER_READ_ONLY_OPTIMAL` as the palette image's current
layout, and a caller that repalettes a job **before its first FILL** would transition from a
layout the image is not in. Harmless in effect (the copy overwrites the whole image) and a spec
violation regardless. Left as it is, named here, because inventing a caller to exercise it is not
this landing's work.

* **A bug in its own new code, found by reading the diff before the review.** `rlist_restart`
  seeded the list from the atlas's entries **bounded by the current allocation**, so an atlas
  holding more entries than the 256 the arm allocates would have lost the tail of its own list —
  silently, which on the other lane is cells that stay indexed for ever with nothing in the log.
  Both the seed and the append now go through one `rlist_room` helper that grows to what is asked
  for or drops the list and says so.
* **The arm set is part of the instrument.** The first run of the sprite oracle measured nothing
  at all: both passes logged `atlas=0` and no restore, because without `native.on=all wrecks` the
  feature pass never owns the leaf, emits nothing and atlases nothing. `feat.on` alone is not
  enough to make a feature atlas exist.
* **The oracle now belongs to the restorer.** The per-job dump moved out of `tagpu_vk_terr.c`
  into `tagpu_vk_restore.c`, so every consumer gets it for free — which is what the next one
  would otherwise have copied. Names are uniform: `tagpu_restore_<tag>.rgba` from the GL lane
  against `tagpu_restore_<tag>_vk.rgba` from this one, and the terrain's GL dump was tagged to
  match. Re-armed on the **paint count** rather than on a serial, because a lazy queue keeps
  painting and a dump is owed again whenever the picture has moved.

#### What the landing review found, and it was all lifetimes and one log line

One HIGH, three MEDIUM, six LOW, all verified against the code before anything was changed, and
**all of them on the lever path or in the notes** — nothing it found can reach a player, because
the shipped path is the read-back and the lever is absent by default.

* **HIGH — the dump's staging was destroyed outside the retire.** `job_free` freed it on the
  argument that it is *"called from a consumer between frames"*, which is not the same as being
  past the fence of the slot that recorded the copy: the window is up to `slots - 1` frames wide,
  and a generation change on any of them destroys a buffer a submitted command buffer still names.
  It goes into the `JRETIRE` that already exists for exactly this, beside the palette staging.
  (Found independently while briefing the reviewer, and reported by it too.)
* **MEDIUM — `rlistRepaint` was a property of the latest GENERATION, and "you must blank" is a
  property of the INTERVAL.** Two resets between two of a consumer's looks collapse into one, and
  `tagpu_feat.c` produces exactly that pair in a single frame: it recycles a full atlas (blank) and
  calls `tagpu_gaf_atlas_restore` on the next line, which repaints if the palette moved. The
  consumer would have been told to keep a destination the GL lane had just cleared. Fixed with a
  monotone **blank counter** published beside the flag; no sequence of generations can hide a
  blank from a consumer that compares it.
* **MEDIUM — "the read-back is then the fallback again" was false.** Both arm latches are one-way
  and arming the list frees the mirror, so after the list's out-of-memory drop there is neither —
  and the consumer went on drawing a frozen twin while the GL lane kept restoring. The comment is
  corrected and the stand-down is made real: the consumer clears its "this twin is a picture" flag
  when the request disappears under a live job.
* **MEDIUM — the same flag survived two permanent abandonments** (the job failing, and a new
  generation arriving after the device had refused), so the pass would sample the old layout's
  twin at the new layout's rects. *"What it painted stands"* is only true until the rects move.
* **LOW ×6**: a `%%s` that printed itself in the one line saying what the lane is waiting for; a
  cursor that advanced by frames *accepted* rather than *offered*, so a frame the restorer refuses
  permanently was re-offered every frame for the life of the atlas; an overflow restart that
  appended the frame it had just re-seeded; a generation-event list that was six events long when
  the code has seven; a stale `tagpu_gaf.c:689` citation this landing's own insertion invalidated;
  and a `cmp` loop in the skill that invites the reader to treat the unit atlas's missing Vulkan
  half as a failure.
* **And it explained the terrain paragraph above**, which is the finding that changed a note rather
  than a line of code.

#### What this does NOT cover

* **The units, which are the fourth consumer and the one with a seam of its own**: the unit
  atlas's twin is **mipped and trilinear**, and its mirror is the whole chain because GL's own
  levels are what make the two lanes byte-identical (§2.43's landing and the gate-3a pass before it). A Vulkan restore paints level
  0 only, so that consumer needs its levels generated on this lane — and a blit chain is this
  fork guessing at `glGenerateMipmap`'s reduction. That is the next landing's question, not this
  one's.
* **The UI atlas** (`tagpu_gui_surf.c`) is a fifth restore consumer and keeps its read-back; it
  has a `restoreMinEdge` floor and its own arm beat.
* **The mid-session lever flip** is correct by construction on both halves now and still has not
  been run.
* **`repaint` remains unexercised** on this lane, for the reason §2.43 records at length: no
  fixture has been found that moves the palette once a level is up.
* **The effects atlas churns during a fight**, and the two lanes' dumps are taken on each lane's
  own "idle": a scene that is still adding frames can therefore be compared at two different
  moments. The comparison above was taken after the fight settled, and that is a property of the
  measurement rather than of the code.
* **The failure paths the review's fixes are about are still unexercised.** The blank counter, the
  cleared "this twin is a picture" flag on an abandoned restore, the out-of-memory list drop and
  the dump's retire were all reasoned to and none of them has been run: the re-measurement after
  the fixes (run 5 above, **91** two-batch slices) proves the steady path still produces the GL
  picture byte for byte, and proves nothing about the paths themselves.
* One GPU, one model (`full`), one map, one fixture per atlas.

### 2.45 What `glGenerateMipmap` actually did to the restored twin, and what that licenses — landing 7e's measurement

The units are the restorer's last unwired consumer and the only one with a seam of its own: their
twin is **mipped and trilinear** (`ATLAS_MIP 2`, `ATLAS_PAD 4`, 2048 square), a Vulkan restore
paints level 0, and the levels have to come from somewhere. `tagpu_gaf.h` has said since gate 3a
that *"a blit chain on the Vulkan side would be this fork guessing at `glGenerateMipmap`'s
reduction, and the guess would be a per-driver difference that no note could pin down"* — so
before writing that landing, **the driver was asked what it actually did**, which is a question
this repository can answer rather than assume.

`tagpu_gaf.c`'s dump now writes the whole chain (`tagpu_restore_unit.mips`, 3 levels of 2048,
21 MB, under the same `tagpu_restoredump.on`), and each level was held against six candidate
reductions of the level above it. `crowd-static`, 155 entries, 101 488 texels of level 1 carrying
art:

| candidate | level 1 exact | level 2 exact | max &#124;Δ&#124; R,G,B,A |
|---|---|---|---|
| **`(sum + 1) / 4`** | **99.61 %** | **99.63 %** | **1, 1, 1, 0** |
| `floor(sum / 4)` | 95.96 % | 96.00 % | 1, 1, 1, 0 |
| round-half-to-even | 96.92 % | 96.97 % | 1, 1, 1, 1 |
| two-pass h then v, round up | 91.78 % | 91.86 % | 1, 1, 1, 1 |
| alpha-weighted (premultiplied) | 96.92 % | 96.97 % | **57, 56, 49, 1** |
| sRGB-aware (linearise, average, re-encode) | 93.40 % | 93.10 % | **54, 45, 45, 1** |

**Two of the six are ruled OUT by the maximum rather than by the average**, which is the reason to
report a maximum at all: alpha-weighting and gamma-awareness are wrong by up to 57 and 54 levels on
a single channel while still matching most texels exactly — 96.9 % for alpha-weighting, 93.4 % for
the sRGB-aware one. An average alone would have called them plausible. (An earlier version of this
paragraph said "97 %" of both; it is 97 % of one of them, and landing 7e-1's review caught the
roadmap repeating the rounder number.)

**What the driver does is an unweighted 2×2 box average of RGBA, with a rounding rule none of the
six reproduces exactly and all of them reproduce to within one level.** Two further facts pin it
down:

* **Alpha is exact** (max Δ 0) under `floor`, `(sum+1)/4` and the two-pass floor — so alpha is
  reduced the same unweighted way, and the disagreement is purely RGB rounding.
* **The differences are on FULLY OPAQUE quads**, not on the mixed-alpha edges: of `floor`'s 42 371
  differing texels at level 1, **42 370 sit on a quad whose four alphas are all 255** and exactly
  one sits on a mixed quad. A different *filter* would have shown up at the edges; a different
  *rounding* shows up in the interior, which is what this is.

Chasing the last 0.4 % to an exact formula was not done: the useful result is the **bound**, and
the bound is **±1 per RGB channel and 0 on alpha**.

**What it licenses, and the choice it puts in front of landing 7e.** Three options, and the
measurement is what makes them comparable rather than a matter of taste:

| | what the Vulkan lane does | what the oracle can claim | what it costs |
|---|---|---|---|
| **A** | keep reading GL's levels back (today's mirror) | `cmp`, byte-identical | keeps the one read-back this plan exists to retire, and it would be the last one |
| **B** | reduce with our own box average | **within 1 LSB on RGB, exact on alpha** — a measured bound, not equality | the plan's strongest instrument weakens from `cmp` to a bounded diff |
| **C** | reduce with our own box average **on both lanes** — `glGenerateMipmap` replaced by the same pass on the GL side | `cmp`, byte-identical **by construction on every driver** | changes the shipped GL twin's minified texels by ≤1/255 per channel |

**C is the recommendation**, and the measurement is what makes it a small decision: the GL side's
own levels already differ from a box average by at most one level, so replacing the driver's
reduction with ours moves no texel by more than 1/255 in art that is being minified — while
removing a **per-driver unknown** from the shipped path and keeping `cmp` as the oracle for all
four consumers. B is the fallback if the GL picture is to stay untouched; A is what the plan would
have done by default, and it is the only one that leaves a read-back behind.

**Not covered:** one driver, one atlas, one fixture. The reduction a *different* GPU performs is
exactly what option C stops mattering and what options A and B leave open — which is the argument
for C stated as a property rather than as a preference.

### 2.46 The twin's levels stop being a driver's rounding rule — landing 7e-1 of the Vulkan-only plan

§2.45 measured what `glGenerateMipmap` does to the restored unit twin and put three options in
front of this landing. **Option C landed, and this is its GL half**: the twin's levels 1.. are
reduced by a pass of ours rather than by the driver, so that the levels the GL lane samples and the
levels a second backend will paint are the same bytes *by arithmetic* instead of by driver luck.

**The pass.** `TAGPU_RESTORE_MIP_FS` (`tagpu_restore_glsl.h`) is the exact integer 2×2 box average
— four fetches, `round(v * 255)` per channel, `(sum + 1) / 4` in integers, written back through the
same `(k + 0.25)/255` trick the OUT pass uses so that a driver which truncates the float-to-unorm
conversion and one which rounds both store exactly `k`. `tagpu_rglsl_mips(tex, dim, mip)`
(`tagpu_restoreglsl.c`) draws it per level into its own framebuffer, and `tagpu_gaf.c`'s
`twin_mips` calls it with `glGenerateMipmap` as the fallback.

**Three things the caller owes the shader, and they are why the fetch is exact rather than nearly
exact:**

* `GL_TEXTURE_BASE_LEVEL` **and** `GL_TEXTURE_MAX_LEVEL` both set to the source level. There is no
  level argument in the shader because the destination is another level of the *same* texture:
  clamping the sampler to the one level being read is what makes rendering into level `L` while
  sampling level `L-1` legal rather than a feedback loop, and it needs no copy.
* the filter pinned to `GL_NEAREST` for the duration. With a non-mipmap minification filter only
  the base level is ever sampled, so the fetch cannot drift to a neighbouring level however the
  implementation computes its level of detail — and NEAREST returns the texel itself rather than a
  bilinear blend that merely happens to weight one texel 1.0.
* the source width in `uSrcDim` rather than `textureSize()`. Both would work; the uniform says in
  the C what the shader reads, and the dimension is a power of two here, so `1.0 / uSrcDim` and
  every texel centre are exact in float.

All four borrowed texture parameters are read back and restored, and the chain is refused whole —
falling back — rather than reduced wrongly if any level of it would be odd: GL's own rule for an
odd level is a weighted three-tap, not a 2×2 average.

**The measurement, on §2.45's own fixture and instrument** (`crowd-static`, 155 entries, the whole
chain dumped to `tagpu_restore_unit.mips` under `tagpu_restoredump.on`):

| level | texels | equal to `(sum + 1) / 4` of the level above | max &#124;Δ&#124; R,G,B,A |
|---|---|---|---|
| 1 (1024²) | 1 048 576 | **1 048 576 — 100.00 %** | **0, 0, 0, 0** |
| 2 (512²) | 262 144 | **262 144 — 100.00 %** | **0, 0, 0, 0** |

For contrast, **our** chain against four other reductions — these are *not* §2.45's figures and
are not comparable with them, because §2.45 measured candidates against the DRIVER's chain and
these measure them against ours: `floor(sum/4)` 96.24 %, `(sum+2)/4` 94.20 %, alpha-weighted
96.61 % (max Δ 57), sRGB-aware 93.23 % (max Δ 54). The driver's chain is no longer on disk — the
reduction replaced it — so §2.45's table is the record of it, and re-taking those figures would
mean standing the reduction down deliberately.

**The three sprite pairs stayed `IDENTICAL`** on the same run (terrain 23 674 880 bytes, features
and effects 16 777 216 each), so landing 7d's oracle did not move.

**What establishes that the reduction actually ran is the 100.00 %, not the absence of a log line**
— and the difference matters enough that the review made it a finding. The driver's own chain was
95.96 % / 96.00 % against the nearest candidate, so a run that fell back could not read 100.00 %;
whereas four of the six ways to stand down were *silent* when this was first written, so an empty
grep proved nothing. All six say so now, once per context and with the reason. Note also that only
**one** twin reduces at all: `twin_mips` returns before the reduction when `mip` is 0, and
`tagpu_render3do.c` is the only caller that sets it — the terrain, feature, effects and UI atlases
are unmipped, so "every twin" would be three twins doing nothing.

**And the unit pass's own A/B still reads 0 px** — `selbox-facings` at 1024×768 with
`classicpp.cfg=assets=1 shadows=1 terrainshadow=1`, **0 differing pixels of 786 432**, 2 125
non-black on each lane. That measurement is the concrete reason option C was taken rather than B:
reducing on the Vulkan side alone would have scattered ±1 through every minified unit texel and
turned this gate from an exact 0 into a permanent small residual.

**Be exact about what that 0 px does and does not prove.** The unit consumer on the Vulkan lane is
still fed by the **mirror** — the whole chain read back off this twin, levels and all — so the two
lanes sample the same bytes today because one copies them from the other, and this A/B would read 0
under either reduction. What it establishes is that changing the reduction did not move the unit
picture. The claim that the two lanes agree **without** the copy is 7e-2's to earn; what landed
here is the arithmetic that makes it earnable.

**A defect this found by reading rather than by measuring, and it was in the first version of the
change.** The twin was created with `glTexImage2D` for **level 0 only**; every other level existed
because `glGenerateMipmap` created it. A reduction draws *into* level `L` through a framebuffer,
and a level with no storage makes that framebuffer incomplete — so the first chain of every twin
would have failed the completeness check, fallen back silently, and a twin painted once and never
again would have kept the driver's chain **for good**. The fix is at creation: every level 0..`mip`
gets its own `glTexImage2D`. The levels are then undefined until the `twin_mips` that follows the
job's creation, and what makes that safe is an **invariant, not the shortness of the window** — the
first version of the comment argued the window ("exactly the two statements"), which was both wrong
(it is six statements including a call) and the shape of argument this project's rules reject.
The invariant is three facts: nothing on that path samples the twin, the only thing that touches it
being the job creation rendering into level 0 to clear it; the one path that abandons the twin
**deletes** it, so no twin with an undefined chain is ever published; and `twin_mips` cannot fail to
write the chain, because `tagpu_gaf.c` demotes `mip` to 0 before every creation if
`glGenerateMipmap` did not resolve, so the fallback is guaranteed to be there when it is needed. (Before this landing the same call was merely
*consistency*; now it is what makes the twin samplable at all. Those are different failures by the
GL specification rather than by measurement: a texture whose chain is incomplete samples as a
defined `(0,0,0,1)`, while a level allocated with `NULL` and not yet written holds whatever the
driver left there.)

**The build gate demanded the Vulkan shader before the Vulkan consumer.**
`tools/spirv-gen.py` refuses a shader in `tagpu_restore_glsl.h` that no program in its manifest
uses, so `restore_mip` is paired and generated here — one small array in
`inc/spirv/tagpu_restore_glsl.spv.h` that nothing `#include`s until landing 7e-2 wires it. That is
the gate doing its job rather than something routed around: the two lanes cannot drift apart while
one of them is unwritten. Adding the row re-hashed the `transform` line of all twelve generated
headers, which is the freshness chain covering the tool's own source.

**Re-measured after the review's fixes**, on the same fixture: the chain is **byte-identical to the
pre-fix run** (`cmp` of the two dumps), still 100.00 % at both levels, the three sprite pairs still
`IDENTICAL`, and **no stand-down line fires on the happy path** — which was the one way the logging
fix could itself have been a bug.

**And the pending-error report fired, once, which is the LOW finding paying for itself in the same
run.** `restoreglsl: 1 GL error(s) were pending before the mip reduction (not ours)` appears
immediately after `restoreglsl: activations 512x512 x 16 layers (128 MB)` — so something in that
neighbourhood leaves the process-wide flag set, and before this landing the reduction consumed it at
frame top with no line. It is **not the reduction's own**: an error raised by the reduction fails it
and prints the other line, which did not appear. The separate slice-bracket report
(`before slice 2`, at startup) is the pre-existing one and is unchanged. Whose it is has not been
chased — it is one line in the log now, and the line before it says where to look, which is the
whole point of not swallowing it.

**Not covered.**

* **The Vulkan lane does not reduce yet.** This landing makes the GL side an arithmetic fact; the
  claim that both lanes agree is 7e-2's to earn, with per-level views on the twin and the oracle
  extended to `cmp` whole chains rather than level 0. The unit consumer on the Vulkan lane is
  still unwired, which is why the unit pair does not appear above.
* **The fallback is still the driver's**, with §2.45's bound: ±1 per RGB channel and 0 on alpha, on
  the one driver that was measured. It is reached only when the reduction cannot build or a level
  is odd, and it says so once in the log.
* **Anisotropy is unchanged** and is still the one sampler difference the port cannot close
  (`aniso=` is read by both lanes for exactly that reason).

### 2.47 The unit twin's whole chain, and the record-time latch that hid an empty atlas — landing 7e-2

§2.46 made the GL lane's levels arithmetic. This landing makes the **Vulkan** lane paint the unit
twin end to end — level 0 through the OUT pass, levels 1..mip through the same integer
`(sum + 1) / 4` — and extends the oracle from a `cmp` of level 0 to a `cmp` of **whole chains**.

**Measured, three consecutive runs on `crowd-static` (2026-09-17):**

| pair | result |
|---|---|
| `terr` | IDENTICAL, 23 674 880 bytes |
| `feat` | IDENTICAL, 16 777 216 bytes |
| `fx` | IDENTICAL, 16 777 216 bytes |
| `unit` **chain** | **IDENTICAL, 22 020 096 bytes — level 0 and both mip levels** |

The unit twin is 155 frames over a 2048 square with `ATLAS_MIP 2`, so the chain is
2048² + 1024² + 512² texels and every one of them matches the GL lane's byte for byte.

**The defect this landing spent most of its length on, because the shape is the lesson.** The
Vulkan lane's first one or two unit batches came out **black** — every texel, alpha 1, eight
near-black values — while every later batch was byte-perfect. The cause was not in the restorer at
all:

* `tagpu_vk_unit_upload` reached `standdown` on its **feed** path — the frame that comes only to
  make the restore job and draws nothing — and `standdown` calls `slot_free`, which is
  `kill_buffer(&s->vstage)`. By that point `atlas_upload` had memcpy'd the indexed mirror into
  that buffer and recorded a `vkCmdCopyBufferToImage` out of it, and `cb` is submitted whether the
  pass draws or not. The source buffer of a pending copy was destroyed under it.
* **The damage is the latch, not the lost copy.** `atlas_upload` sets `s_atHave`, `s_atSerial` and
  `s_atRows` at **record** time, so after the lost copy the pass believes the device holds rows it
  never received, and it will not re-upload until the mirror's serial moves again. `restore_want`,
  three lines below the upload, then hands the restorer frames `covered_prefix` calls covered.
* The restore therefore ran against an **empty** atlas image. OUT computes `frag = c - net`, and
  with the source reading 0 both `c` and `net` come from palette index 0, so the cell is black.
* **Only the Vulkan lane can see it.** The GL lane samples `tex`, which was never missing the art.

Fixed by construction — a **lifetime**, not a timing: the feed path leaves through `feedout`, which
destroys nothing, exactly as `refuse` does and for the reason `refuse` already stated. Every other
`goto standdown` is above `slot_vstage`; the label's comment said "safe here and only here", which
reads as a property of the label rather than of its callers, and both comments now name the side of
the staging they are safe on.

**What the measurements ruled out first, recorded so nobody re-walks it.** Each of these was a
plausible reading of "the two lanes restored the same bytes differently" and each is wrong:

* the activation arrays' growth, the descriptor generations, the framebuffer cache and the retire —
  pinning the arrays to 512 square so they are created once and never re-created reproduces the
  failure **byte-identically**;
* the CPU mirror — per-frame non-zero counts taken at upload time give `empty=0` of 25 listed
  frames;
* the staging memcpy — the staged bytes are byte-for-byte the mirror, 55 608 == 55 608;
* the palette — it is set once, before the job exists, and never moves.

**What identified it** was making the OUT shader report the index it had read, on *both* lanes: only
the R channel differed, GL read the art and Vulkan read **0 everywhere**. Then reading the device
image back **early** showed 404 798 texels missing where the same read-back taken later is exact.

**The oracle line that hid it, and this is worth more than the fix.** `unit SOURCE: IDENTICAL`
compares the GL texture against the Vulkan device image **after everything has settled**, so it
cannot see a source that was empty while the lane was restoring. It reported "the two lanes restored
the SAME bytes differently" and the premise was false. A dependent-lane oracle must compare the
lanes' **inputs at the moment of use**, not at the end of the run; when a dependent lane's picture
is wrong, make the shader report its input before theorising about its arithmetic.


**The review's four code findings, all verified against the source and all acted on.** Three of
them are about paths this driver never takes, which is exactly why they needed a reader rather than
a run:

1. **A level-0-only twin drew no units at all, for the session.** `atlas_rgb_build` created the
   per-level views only for `mips >= 1`, but the OUT pass paints *through* `s_arLvl[0]`, so with
   `restoreMips == 0` the job was created with a null `dstView`; `job_new` refuses that **without a
   word**, `s_rjTried` is cleared only by a teardown, and the pass then stood down on every frame.
   `restoreMips == 0` is not hypothetical — `tagpu_gaf.c` demotes the atlas's `mip` to 0 whenever
   `glGenerateMipmap`/`glTexParameterf` do not resolve, and the build site admits `>= 0` by name. So
   the views start at level 0, the prerequisite gate is unconditional, and the refusal says so.
2. **The "twin moved under a live restore" guard could not fire in two of the cases it exists for**
   — it tested `s_arLvlN > 0`, and `atlas_rgb_build` leaves that 0 on precisely the paths that
   destroy the image and every view over it. It was also in the wrong place: it runs later in the
   frame than the destruction it guards against. The job is now dropped by `atlas_rgb_build` itself,
   before `kill_image`, so its framebuffers retire on the mask a submitted command buffer is bound
   by; the check in `restore_want` stays as a backstop and can now actually fire.
3. **The forced first mip reduction sampled level 0 while it was still `UNDEFINED`.** `chain_step`
   ran one pass over an unpainted twin, on the stated grounds that the levels begin undefined and
   the consumer samples trilinearly — and both halves were wrong: the consumer does not sample the
   chain until `painted > 0`, and level 0 is undefined too until `dst_ready` transitions it in the
   OUT path. A descriptor declaring `SHADER_READ_ONLY_OPTIMAL` over an `UNDEFINED` image is
   undefined behaviour, and it fired on the *ordinary* first frame.
4. **A degenerate list frame stalled the cursor for good.** `covered_prefix` returned at `h <= 0`
   as well as at "not yet uploaded", conflating a frame the restorer will refuse with a coverage
   boundary — so every frame behind it stayed unpainted while `s_arHave` still called the twin a
   picture. It answers the coverage question alone now, which is what makes `restore_want`'s own
   "a frame the core can never queue is skipped for good" true.

The chain re-measured `IDENTICAL` after the fixes, on both fixtures.

**Not covered.**

* **The same record-time latch is still there in `tagpu_vk_feat.c` and `tagpu_vk_fx.c`.** Their
  `atlas_upload` equivalents set their have/serial/rows the same way, and neither has a feed path
  reaching a slot-freeing stand-down today — so the hazard is unreachable in those two rather than
  absent, and their pairs are identical for that reason and not because the pattern is sound. Any
  new stand-down added below their staging allocation re-opens it.
* **`tagpu_vk_restore_job_repalette` still has no caller** while `tagpu_rglsl_job_repalette` is
  called from `tagpu_gaf.c`. *Both are deleted since, and since G20c the world does not repaint
  on a Gamma move at all: every world source is the unscaled table and the factor is applied once,
  at the world composite (§2.3f).*
* **`repaint`, the blank counter, the cleared-picture flag and the out-of-memory list drop remain
  unexercised**, as does the dump's retire. `job_clear` has no callers.
* **Anisotropy is unchanged** and is still the one sampler difference the port cannot close.
