# Log — the passes ported to Vulkan (G19d–G19f)

The G19 port of each pass to the Vulkan lane, landing by landing.

*A frozen record: nothing new is written here. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.26 The FPS readout, drawn by Vulkan (`tagpu_vk_fps.c`, `tagpu_vk_pass.h`, `tagpu_vk_shot.c`) — Phase G, G19d

The first pass of the renderer to run end to end on the second backend, and the smallest one that
exercises a buffer, a texture, a shader and a draw with nothing depending on it.

**No engine address, and no new one was read to build it** — so there is nothing for
[exe reverse engineering](../exe-reverse-engineering.html) in this landing, and nothing for the hook
map above either. Neither new file is on `thread-split.allow` and neither may ever need to be
(Phase G standing constraint 1); the list is **unchanged** by this landing, at the 34 entries
`thread-split-check.sh` reports. Everything either one uses arrives
as an argument: the frame's vertices from `tagpu_fps.c`, the atlas from `tagpu_text.c`, the device
and the render pass from `tagpu_vk.c`.

**MEASURED 2026-09-15**, on `scenarios/selbox-facings` under system wine on the reference setup's
4070: **0 differing pixels of 786 432 at 1024×768 and 0 of 2 073 600 at 1920×1080** — 89 and 92 ink
pixels on each side, and the two capture files are byte-identical. **0 again after the lever was
cleared and re-armed**, which exercises the whole teardown and rebuild.

**It is not a second implementation of the pass**, and that is what makes the number mean anything:

| | where it comes from |
|---|---|
| the vertices | `tagpu_fps_quads` — the quads the GL lane just drew, handed over **exactly once** |
| the texels | `tagpu_text_atlas` — the same 128 KB `tagpu_text_tex` uploads to GL |
| the shader | `inc/spirv/tagpu_fps.spv.h`, generated from the GL string by §2.25 |
| the frame size, the ink | the same numbers |

So what the comparison compares is two RASTERISERS. If this file rebuilt the quads, 0 px would
only mean two pieces of arithmetic agreed.

**Handing the vertices over ONCE, and exactly what that buys.** The overlay does not run on every
path that reaches the swap. Consuming makes it impossible for one frame's vertices to be drawn
TWICE, which is the case that matters: the second draw would be of a readout already superseded.
It does *not* make it impossible for the lane to draw the newest vertices there are on a frame
`tagpu_fps.c` was skipped on — that frame shows a readout one frame stale, which is a digit and
not a fault. **This paragraph claimed the stronger property until the landing's review disproved
it**; making it true would need a frame stamp the two files share, and nothing yet needs one.

**The Y flip is pipeline state, never a source edit.** GL's clip space has +Y up and Vulkan's has
+Y down, so the vertex shader — byte-identical to the GL one below its declarations — puts the
readout at the bottom of the frame, mirrored. The fix is a **negative viewport height**
(`VK_KHR_maintenance1`, asked for by name in `vkCreateDevice` and the pass refuses to arm without
it, saying so). Flipping the geometry instead would mirror every glyph, because the texture
coordinates travel with the vertices; flipping the shader would make it disagree with its oracle.

**The atlas upload waits for the device**, and that is a fence rather than a hope — but its cost is
real and is stated rather than implied. The atlas changes when a string is rasterised into it for
the first time, about twenty times in a session, and by then earlier frames may still be sampling
the image; a barrier in this command buffer orders nothing about submits already in flight. So the
upload calls `vkDeviceWaitIdle` first and re-sends the whole image from
`VK_IMAGE_LAYOUT_UNDEFINED`. **What that costs:** this is the render thread, the game thread waits
INFINITE on it across a mode change, and `vkDeviceWaitIdle` takes no timeout — so the stall is the
drain of two or three frames in flight on a healthy device, and as long as the wedge on a wedged
one, which is the exposure the lane's own fence wait bounds at a second and this one does not. The
by-design alternative is a second image, swapped when every slot has turned over, which moves the
same in-flight problem up to the descriptor sets for twenty events a session. **§2.28 answered
that for a pass that uploads every frame, and the answer was simpler than a swap**: one image per
FRAME SLOT, which the seam's fence already proves free, so there is no in-flight problem to move.
This file was not changed to match — twenty stalls a session buys nothing back, and the readout's
atlas is 128 KB against a viewport. **A failed wait is not an upload**: the
generation is left unclaimed and the lane comes down on the next fatal result.

**One buffer set per FRAME SLOT**, and what proves slot *i* is free is the seam's fence wait at the
top of its present — not a frame count, not "the GPU will have finished by now". `TAGPU_VK_SLOTS`
is defined once in `tagpu_vk_pass.h` and `tagpu_vk.c`'s `MAXIMG` is defined from it, so the two
cannot drift.

**`tagpu_text.c` gained its own counter, and it is not `s_dirty`.** There are two consumers of that
128 KB now, and `s_dirty` is CONSUMED by the upload that reads it — whichever consumer ran second
would never see a raster land. `tagpu_text_atlas(&gen)` returns a counter that ticks when the
PIXELS change and is never cleared; it deliberately does not tick in `tagpu_text_glreset`, because
a lost GL context does not change a byte of the atlas.

**The seam is still one file.** `tagpu_vk_pass.h` is what a ported pass is handed: an instance, a
physical device, a device, a render pass, a slot count, the flip flag and the two `GetProcAddr`s
— and, for one user only, the seam's queue and its family (`queue`, `qfam`), which
`tagpu_vk_stage.c`'s banded upload submits to from the same render thread (§2.88, *The
staging*). Nothing in it names a window, a surface or a swapchain, so `tagpu_vk_fps.c` would draw into an
offscreen image or another process's image unchanged — standing constraint 3 holding rather than
asserted. **Every pass resolves its own entry points**: the presentation table (swapchain, acquire,
present, fences) and a pass's (pipelines, descriptors, buffers, images) barely overlap, and one
shared table would have to be the union of every pass ever written.

**What changed inside `tagpu_vk.c`.** A `VkRenderPass` per DEVICE — one colour attachment in the
swapchain's format, `initialLayout` `TRANSFER_DST_OPTIMAL` (which is what `vkCmdClearColorImage`
left it in) and `finalLayout` `PRESENT_SRC_KHR`, so it performs both transitions the old code did
by hand, ordered by an external subpass dependency from `TRANSFER` to `COLOR_ATTACHMENT_OUTPUT`.
It is per device rather than per swapchain so that a pass's pipeline survives every resize; the
image views and framebuffers follow the swapchain, and a rebuild that came back with a different
format brings the lane down and says so rather than drawing into a lie. `prepare` runs OUTSIDE the
render pass and `record` inside it, because a texture upload is a transfer and a transfer may not
be recorded inside a render pass.

**A synchronisation fault found while adding it, and fixed.** The acquire semaphore was waited on
at `COLOR_ATTACHMENT_OUTPUT` alone — while the first thing the command buffer does to the image is
`vkCmdClearColorImage`, which runs at `TRANSFER` and is **not** later in the pipeline order. So the
clear was free to run before the acquire had handed the image over: a write to an image the
presentation engine may still be reading. It does not throw; it tears a frame now and then on a
driver that happens to overlap. The mask is now `TRANSFER | COLOR_ATTACHMENT_OUTPUT`.

**The oracle, and why it is not `tacli glshot`.** Route D gives Vulkan its own window, so nothing
on the GL side can see what Vulkan drew — only Vulkan can. `tagpu_fps.ab` makes both lanes capture
one frame over a black field: the GL pass clears the frame, draws, reads it back with
`glReadPixels` and writes `tagpu_fps_gl.ppm`; the lane copies the swapchain image it just presented
into a host-visible buffer and writes `tagpu_fps_vk.ppm` (`tagpu_vk_shot.c`, which allocates the
buffer when the lever fires and gives it back as soon as the file is written — 8.3 MB at 1080p, in
a 32-bit address space). `tools/vk-ab.py` diffs the two and **refuses two captures of different
sizes** rather than scaling one, because a scaled comparison cannot be 0 px by construction.
Set `color=0,0,0` in `tagpu_vk.on` so the two backgrounds match.

**What the teardown rests on, said once.** `vk_down` and `vk_resize` free everything behind a
`vkDeviceWaitIdle`, and that call CAN fail — `VK_ERROR_OUT_OF_HOST_MEMORY` above all, in the
32-bit address space this lane spends its budget measuring — returning without the device being
idle. The capture's staging buffer is the one thing whose ownership this landing newly made rest
on it, so its result is read there: a wait that did not succeed **leaks** the buffer instead of
freeing it under a copy that may still be running, which is the same trade `ST_ZOMBIE` makes (a
leak is recoverable, a free is not), it is logged, and it costs at most one capture for the
session. The rest of that teardown — the swapchain, the per-image objects, the device — has always
rested on the same wait and is unchanged here; naming it is not fixing it.

**The capture adds no wait, and the swapchain images are created so that it is legal.** Two things
the review changed, both real. The copy is recorded into frame slot *i* and completed by the fence
the seam **already** waits on at the top of the next frame that reaches slot *i* — `nimg` frames
later, a few milliseconds, no second mechanism and nothing that can free a buffer the GPU still
owns. And the swapchain is created with `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, gated on
`caps.supportedUsageFlags` so that a surface refusing it loses the capture and keeps the lane:
`vkCmdCopyImageToBuffer` requires that usage at image CREATION and no layout transition confers
it, the reference ICD allowed it anyway, and with no validation layer running the 0-px result
rested on undefined behaviour until this was fixed.

**The lever puts back every piece of GL state it touches** — the clear colour, the scissor enable,
`GL_PACK_ALIGNMENT`. It did not, and a lever that exists to measure the renderer is the last thing
that should change it.

**Both captures are of the same frame, or there is only one of them.** The first shape had each
lane poll the lever for itself — and they poll on different cadences (30 frames against 250 ms)
while the readout changes its number twice a second, so the two could have landed hundreds of
frames apart and differed in the digits while agreeing about everything else, which reads exactly
like a broken port. The flag travels WITH the vertices now, and the Vulkan pass claims it only
after every reason not to draw is past: a frame the pass cannot draw loses its half of the pair
rather than capturing a bare clear against a GL half that has text, which would have reported every
text pixel as differing — a port failure that is really an oracle failure. *(This paragraph said
"by construction" and the review's second pass disproved that too: the flag was claimed the moment
the quads arrived, with four `return 0`s still ahead of it.)*

**Constraint 4, measured rather than argued.** With `tagpu_vk.off`, this DLL's GL frame against
main's (`a043b05`) on the same fixture, same camera, pointer parked in the same place: **46
differing pixels of 786 432, all of them inside x 25..43, y 8..14** — the DIGITS of the frame-rate
readout, two processes running at different rates. Every other pixel is identical.

**What the pass allocates**, sized by the swapchain's image count (four on the reference setup):
one host-coherent vertex buffer of **18 KB** (`TAGPU_FPS_MAXV` = 288 vertices × four floats × four
slots), one **512-byte** uniform buffer (a 128-byte stride a slot, which is
`minUniformBufferOffsetAlignment` × 2), a 512×256 `R8_UNORM` device-local image (**128 KB**) and a
128 KB staging buffer — four allocations, persistently mapped where they are host-visible. Too
small to show in the lane's VA figures, which are unchanged at **+6.4 MB** peak committed
(95.9 → 102.3 MB) with the largest free block still **247.4 MB**.

**NOT COVERED.** No Vulkan **validation layer** ran — none is installed in the wine prefixes — so
the barriers, the stage masks and the layout transitions are argued from the specification and
from a correct picture, not verified by a layer. The stage-mask fault above is what that gap looks
like when it bites, and it was found by reading rather than by a tool. Also not covered: any
resolution but 1024×768 and 1920×1080, any device but the 4070, and Windows.

### 2.27 What G19c + G19d's review changed — 2026-09-15

Two independent read-only reviewers on `main...HEAD`, one on correctness and one on
synchronisation and lifetime alone. **Both found the same two HIGH defects independently**, which
is the strongest thing that can be said for running two.

| | what it was | why it mattered |
|---|---|---|
| **HIGH** | the swapchain images were never created with `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, and the capture copied from them anyway | `vkCmdCopyImageToBuffer` requires that usage at image CREATION; no layout transition confers it. The reference ICD allowed it and no validation layer was running, so **G19d's entire 0-px result rested on undefined behaviour** — on another driver the oracle reads garbage or faults. It is asked for against `caps.supportedUsageFlags` now, and a surface that refuses it loses the capture rather than the lane |
| **HIGH** | the capture waited up to a second on the frame's fence and, **on the timeout**, destroyed the staging buffer the already-submitted copy writes into | the GPU would then have completed a copy into freed memory, so the one-second timeout was load-bearing rather than the belt the comment claimed. It is also exactly the shape [CLAUDE.md](https://github.com/Code-Herder/ta-impure-patch/blob/main/CLAUDE.md) forbids — a correctness argument made of timing. There is no wait at all now: the copy is completed by the fence the seam **already** waits on at the top of the next frame that reaches that slot |
| MEDIUM | the A/B flag latched when the Vulkan lane never consumed it | set once, cleared only on consumption — so on a frame that did not reach `tagpu_vk_fps_prepare` (the lane not yet `ST_READY`, a swapchain rebuild, the lever off) the flag rode a LATER frame's vertices and the two captures were of different frames. Which is the one thing the design exists to prevent, and §2.26 said so. Its life is one frame now |
| MEDIUM | the freshness gate hashed the GLSL and not the SPIR-V | a hand-edited, truncated or badly-merged word array passed `--check` and `-fsyntax-only` and would have shipped. A `words` hash per shader closes it; verified by corrupting one word |
| MEDIUM | `VK_INCOMPLETE` was treated as a failure when enumerating device extensions | a driver with more than 512 extensions returns it with a perfectly good partial list; the lane would have reported that `VK_KHR_maintenance1` "is not offered" on a device that offers it and refused **every ported pass for the session**. The list is counted and allocated now, and `VK_INCOMPLETE` is an answer — which also takes 130 KB off a worker's stack |
| MEDIUM | `vkDeviceWaitIdle` in the atlas upload was described as costing nothing | the ordering is sound; the cost is a render-thread stall with no timeout, on the thread the game thread waits INFINITE on. Stated now, with the numbers and with the alternative that was not taken and why |
| LOW | the capture's barrier was sourced at `COLOR_ATTACHMENT_OUTPUT` | the render pass performs its `finalLayout` transition at `BOTTOM_OF_PIPE`, which is later, so the barrier was not ordered after it |
| LOW | the A/B lever left the clear colour, the scissor enable and `GL_PACK_ALIGNMENT` changed for the session | small in practice, and precisely the kind of leak from a measuring lever into play state that an A/B six months from now would be reading |
| LOW | `spirv-check.sh` reported "could not run" (exit 2) as "stale" (exit 1) | it would send someone to regenerate headers that are perfectly current |
| LOW | the extractor skipped a shader whose literal began on the declaration's own line | no shader does that today, and a tool that silently SKIPS one is worse than a tool that refuses it — it would escape the "a shader no program uses" guard too. A census makes it an error |
| LOW | `s_abShot` survived a teardown | benign, and gone with the wait it belonged to |

**Three claims in the notes were findings in their own right** and are corrected in place: that the
vertex hand-over made staleness impossible "by construction" (it makes a *repeat* impossible; a
frame the overlay skipped draws one-frame-stale digits), that the staging buffer was "owned by the
GPU whatever the present then said" (it was, which is why freeing it on the timeout was the bug),
and that the freshness gate covered the generated headers (it covered their input).

**What survived verification without a finding**, because it is worth knowing which parts were
actually checked: the std140 offsets — one reviewer decoded `OpMemberDecorate … Offset` out of all
thirty-four committed arrays and compared them to the comments, **0 mismatches**; the descriptor
layout against the SPIR-V's own decorations; that `fi = frame % nimg` with the fence wait on
`fence[fi]` really does prove that slot's buffers free (a bound, not timing); that
`tagpu_vk_fps_down` gives back exactly what was built including the mapped-but-failed path; that
no handle or entry point from one `VkDevice` can reach another; `tagpu_text.c`'s generation
counter; `tools/vk-ab.py`'s parser and the agreement of its verdict with its exit status; and that
the diff contains no engine address and no byte patch.

**A SECOND PASS on the rework** — because two of the fixes changed the synchronisation design
rather than patching it, which is the class that ships silently. It confirmed both HIGH fixes hold
and found five more, one of them the rework's own:

| | |
|---|---|
| the rework's own | `vkDeviceWaitIdle`'s result was discarded in `vk_down` and `vk_resize`, and that result had just become the WHOLE safety argument for freeing the capture's staging buffer — the same defect one call further along. It is read now, and a wait that did not succeed leaks the buffer rather than freeing it under a live copy |
| widened by the rework | the A/B flag was claimed the moment the quads arrived, with four `return 0`s still ahead of it (one of them added by the rework), so a frame the pass could not draw would have been captured as a bare clear against a GL half with text — every text pixel reported as differing, a port failure that is really an oracle failure |
| | the state save and restore added three entry points to the A/B's list, and a context missing one made the lever do nothing at all, silently, retrying every poll for the session |
| | a failed row-buffer allocation left a header-only PPM on disk, which reads as a broken writer rather than as a machine out of memory |
| | the words hash was keyed off the comment above each array rather than the array's own symbol, so a header whose two had drifted apart could hash one shader's words under another's name |

Its verdict on the two HIGH fixes, which is the reason the pass was run: `TRANSFER_SRC` **holds**
(`cansrc` is re-derived on every swapchain creation, `vk_down`'s memset fails it closed, the single
use site is the caller so a refusing surface loses the capture and not the lane); the capture
lifetime **holds** on every enumerated path — lever cleared, any fatal result, `OUT_OF_DATE`,
`SUBOPTIMAL`, mode change, window destroyed, GPU row changed, `ST_ZOMBIE`, render-thread stop —
"nothing leaks; nothing frees early except through the unchecked waitIdle", which is the finding
above and is now closed.

**Still not covered, and unchanged by either pass:** no Vulkan validation layer ran. Every finding
above was found by reading the specification against the code. The first HIGH is what that gap
costs when nobody is reading.

### 2.28 The scaffold overlay, drawn by Vulkan (`tagpu_vk_scaffold.c`, `tagpu_abshot.c`) — Phase G, G19e

The first of the **world passes**, and the one chosen first because it is the smallest of them
(a fragment stage of 430 words, one sampler, one 16-byte uniform block) *and* because it forces
the question G19d was allowed to leave open: it uploads a **viewport-sized image every frame**,
where the readout's atlas changes about twenty times in a session.

**No engine address, and no new one was read to build it** — so there is nothing for
[exe reverse engineering](../exe-reverse-engineering.html) in this landing and nothing for the hook
map above. Neither new file is on `thread-split.allow` and neither may ever need to be (Phase G
standing constraint 1); the list is **unchanged** at the 34 entries `thread-split-check.sh`
reports. Everything both files use arrives as an argument.

**MEASURED 2026-09-15**, on `scenarios/feat-forest` (151 tall features in the sweep) under system
wine on the reference setup's 4070: **0 differing pixels of 786 432 at 1024×768 and 0 of
2 073 600 at 1920×1080**, with **190 247 and 377 020 non-black pixels on *each* side** — a
quarter of the frame is scaffold ink, so this is not two blank frames agreeing, which is the
failure mode `tools/vk-ab.py` exists to name. The capture files are byte-identical. **0 again
after the lever was cleared and re-armed**, and **0 again after a full free-and-rebuild of every
per-slot resource inside one lane** (below).

**It is not a second implementation of the pass:**

| | where it comes from |
|---|---|
| the scaffold bytes | `tagpu_scaffold_overlay` — the buffer the GL lane just uploaded to its own texture, handed over **exactly once** |
| the quad | `TAGPU_SCAF_QUAD` in `inc/tagpu_scaffold.h`, **one literal both lanes build their vertex buffer from** |
| the NDC rect, the row count | the numbers the GL draw passed to `uRect` and `uRows` |
| the shader | `inc/spirv/tagpu_scaffold.spv.h`, generated from the GL string by §2.25 |

#### The per-frame upload, which is this landing's real work

§2.26 named the by-design alternative to `vkDeviceWaitIdle` and left it to this gate. It turned
out to need **no new mechanism at all**:

> **One image and one staging buffer per FRAME SLOT**, and the seam's fence is what makes writing
> them safe.

The invariant is the one `tagpu_vk_pass.h` already publishes for buffers, used for an image:
when `prepare` is called with `slot`, the seam has just waited on `fence[slot]`, so the submit
that last used that slot's resources has **completed**. Nothing of ours is in flight for it.
Three things follow for the same one-line reason, none of them needing a device-wide wait:

* the CPU may write that slot's staging buffer — a barrier could not help here anyway, because
  it orders GPU work and the hazard is a CPU write;
* the copy into that slot's image has no prior access to order against, so `oldLayout` is
  `UNDEFINED` (the whole image is re-sent, so there are no contents to preserve) and
  `TOP_OF_PIPE` with an empty source access mask is correct rather than lazy;
* the descriptor set naming that image may be rewritten, which is what makes a **viewport change
  free**: the slot is rebuilt at the moment we are handed it, which is the moment we own it. No
  retire list, no deferred free, no second wait. (The game's viewport changes without the
  swapchain changing — a shell/game transition alone does it.)

**What it costs, and the viewport is not the screen.** Measuring that rather than assuming it
halves the figure: a 1024×768 screen has an **896×704** viewport, so 630 784 bytes a slot, 2.4 MB
of device-local and as much again host-visible, **4.8 MB** in all on the reference setup's
four-image swapchain; 1920×1080 has **1792×1016**, so 1 820 672 a slot and **13.9 MB**. The
image's own allocation is whatever `vkGetImageMemoryRequirements` asks for an optimal-tiled R8 of
that extent, so at or above those numbers.

**And none of it is kept once there is nothing to draw.** An early `return 0` when the GL twin
publishes nothing held every byte of that for the life of the lane after one look at the
scaffold — *the first version of this file claimed the opposite in its own header, and the claim
was false*. Slot `slot` is given back at that point for the same reason and at the same instant
the rest of the function writes it in, so after one turn of the slots the pass holds only its
pipeline, its sampler, its descriptor sets and a 512-byte uniform buffer, none of which scales
with anything. The lever cleared, the shell, a level teardown and a frame the twin skipped all
arrive there. The cost of being wrong is one `vkCreateImage` a slot when the pass comes back, and
that happens at a shell/game transition, not in a frame.

**The cheaper alternative, and why it is not here.** One image shared by every slot is *also*
correct: a barrier at the top of each upload, `FRAGMENT_SHADER`/`SHADER_READ` →
`TRANSFER`/`TRANSFER_WRITE`, orders the copy after the previous frame's sampling, because
submission order spans submits to one queue and a write-after-read hazard needs only an execution
dependency. It saves `(nimg−1)` images — 5.2 MB at 1080p — and costs three things: the image's
layout has to be tracked across frames, a viewport change has to retire the old image until every
slot has turned over, and the safety argument goes from one line to a paragraph about cross-submit
ordering. The staging buffers stay per-slot either way. **That is the design to reach for if a
later per-frame pass needs the memory back**; for the first of them, the one-line invariant is
worth 5.2 MB behind a lever.

#### The oracle is shared now, because a world pass has a background

`tagpu_abshot.c` is the GL half of every Phase G A/B, and the three world passes still owed one
get it in three lines. G19d could black the whole GL frame because the readout is the **last**
thing drawn; a world pass has the rest of the frame under it on the GL side and a bare clear
under it on the Vulkan side. So the GL twin blacks the frame **immediately before its own draw**
and reads it back **immediately after**, before the native pass and the UI layer have run. What
is compared is one pass over black against one pass over black. The player sees one frame with
everything drawn before that point missing from it; that is what a measuring lever costs, and it
is why it is one frame.

`tagpu_fps.c` is refactored onto it and **loses 111 lines** (501 → 390) — its own A/B still measures **0 px of
786 432** on the refactored oracle, which is the regression that says the move was clean.
*(This said "117 lines (501 → 384)" until the G19e re-review counted it: 384 was the file before
`fecd146` added the six-line depth-state block, and the figure was never re-taken. The ink count
that used to be quoted here is gone on purpose — those pixels are the readout's own digits, so the
number follows the frame rate on screen and is not a property of the port; see §2.30.)* One thing improves rather than moves: the review of G19d found that a context
missing any of the A/B's entry points made the lever do nothing **silently**, retried every poll
for the session. There is now one resolve, one message naming the entry point that was missing,
and one latch. (The five it needs — `glGetFloatv`, `glIsEnabled`, `glDisable`, `glClearColor`,
`glReadPixels` — are not among the fork's own globals in `opengl_utils.h`; this DLL is
`ddraw.dll` and does not link opengl32, it loads it, so even GL 1.1 is a pointer to resolve.)

#### Two ported passes made a new way to get a plausible wrong answer

Each GL capture holds **one** pass, because its twin blacks the frame around its own draw. The
Vulkan capture is **one frame**, and a frame holds every armed pass at once. So with two A/B
levers armed the diff would report the other pass's pixels as differing: a port failure that is
really an oracle failure, which is the worst answer an oracle can give.

The seam refuses to capture when **more than one pass claimed the frame, or more than one drew
into it**, and says so in the log. Both conditions are checked, and the second is the one that
does the work: verified 2026-09-15 by arming `fps.ab` and `scaffold.ab` together, the two levers
landed on **different** frames (their poll cadences differ) so each claimed alone — and the frame
was contaminated anyway, because both passes were drawing. A claim-count check alone would have
written the pair and been believed.

#### Pipeline state: blending is new, depth is not needed, and one of them is G19e's next question

**Blending** is set on **both factor pairs** (`SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` for colour *and*
alpha), because `glBlendFunc` sets the alpha factors as well as the colour ones. The shader
writes a 0.55 alpha and `discard`s where the scaffold is free, so over the A/B's black field the
result is 0.55 × the ramp colour on each lane — and the 0-px result is what says the two blend
units agree to the least significant bit. The Y flip is a negative viewport height exactly as in
§2.26, and there is no culling, because that flip reverses every triangle's winding. The topology
is a **strip**, which is what the GL twin draws: four vertices as a strip and six as a list cover
the same quad but not necessarily with the same two triangles, and a diagonal running the other
way puts every pixel on it on the other side of a rounding decision.

**Depth is not in this landing** — the GL twin calls neither `glEnable(GL_DEPTH_TEST)` nor
`glDepthMask`, and the seam's render pass has one colour attachment and no depth one. The passes
that **do** test are G19e's next landings, and the question is stated here rather than guessed at
when one of them starts:

> GL maps clip z from [−1, 1] onto the depth range and Vulkan takes [0, 1] directly. Every one of
> these shaders already writes a z in [0, 1], so under GL the near half of the range is thrown
> away and under Vulkan it is not. **The ordering is identical either way** — both mappings are
> affine and increasing, so every depth *test* comes out the same — but the *values* are not, and
> Vulkan gets a bit more precision out of a fixed-point buffer than GL does, which is enough to
> settle a z-fight the other way and put a 0-px comparison out of reach.
>
> The fix is **pipeline state, never a shader edit** (an edited shader would disagree with the
> twin that is its oracle), and it needs **no extension**: a viewport with `minDepth = 0.5` and
> `maxDepth = 1.0` maps clip z ∈ [0, 1] onto exactly GL's `(z+1)/2`, values and precision
> included. `VK_EXT_depth_clip_control` with `negativeOneToOne` would do the same by adopting GL's
> convention outright, and is the answer if a shader is ever found that writes a z below 0 — Vulkan
> clips those and GL does not. **Neither is written yet**, and the render pass will need a depth
> attachment before either matters.

**Both halves of that were BUILT by the next landing and the answer held** — §2.29. The render
pass carries a depth attachment (`tagpu_vk.c`, one image per swapchain image, `LOAD_OP_CLEAR` at
1.0), and the viewport carries `minDepth 0.5 / maxDepth 1.0`. The feature pass is 0 px against its
GL twin with them and the scaffold's own A/B still reads 0 px, so the attachment cost the passes
that do not test nothing. `VK_EXT_depth_clip_control` is still unused and still the answer only if
a shader is found writing a z below 0.

**NOT COVERED.** No Vulkan **validation layer** ran — none is installed in the wine prefixes — so
the barriers, the stage masks and the layout transitions are argued from the specification and
from a correct picture. Also not covered: any resolution but 1024×768 and 1920×1080, any device
but the 4070, and Windows.

#### Constraint 4, measured — and a fixture correction worth more than the measurement

**0 differing pixels of 630 784** in the world viewport, this landing's DLL against `afceba5`'s,
both with `tagpu_vk.off`, on `selbox-facings` at 1024×768, sim paused (`tab`), pointer parked in
the side panel so its animating sprite is outside the crop.

**`selbox-facings` does NOT have a 0-px cross-launch floor at this camera**, and the note that
says it does is about a *within-run* floor. Two launches of the **same** DLL differ by **71 px at
x 1017..1023, y 236..277** — a two-state artefact in a 7×42 sliver at the frame's right edge,
which reproduced DLL-against-itself and is therefore nobody's change. It was caught only because
the first base-versus-landing pair happened to land in the same state and read 0; a second sample
of the same binary read 71. **Take at least two samples of one binary before believing a
cross-launch floor**, and mask that sliver on this fixture.

`feat-forest` is worse and cannot answer the question at all: its walking commander gives it a
**~4000 px** cross-launch floor (3985 px, same DLL, two launches), which is *larger* than the
3906 px a base-versus-landing pair reads there. It is an excellent A/B fixture for the two lanes,
because both captures are of the same frame, and a useless one for anything across launches.

Also worth knowing for any cross-launch diff on a scenario: the engine's own "**forces have been
obliterated**" messages from `clear_existing` sit in the top-left of the viewport for tens of
seconds and land differently per run — 8179 px until they expire. 35 seconds after the load is
enough — **but only if the game is RUNNING**: §2.29 found they expire on TICKS, so a `tab` sent
straight after the load freezes them on screen for as long as the pause lasts.

### 2.29 The feature pass, drawn by Vulkan (`tagpu_vk_feat.c`, `tagpu_gaf.c`'s CPU mirror) — Phase G, G19e

The **second world pass**, and the first one that **depth-tests** — which is why it went next: it
forces §2.28's depth decision, which was settled on paper and left for whichever pass needed it,
into pipeline state that a 0-px comparison can grade.

**No engine address, and no new one was read to build it** — the pass takes everything through
`tagpu_feat_handover`, so there is nothing for [exe reverse engineering](../exe-reverse-engineering.html)
in this landing and nothing for the hook map above. `tagpu_vk_feat.c` is not on
`thread-split.allow` and may never need to be (Phase G standing constraint 1); the list is
**unchanged** at the 34 entries `thread-split-check.sh` reports.

**MEASURED 2026-09-15**, on `scenarios/feat-forest` (151 tall features, 180 body and 150 shadow
drawables in the sweep) under system wine on the reference setup's 4070, `ss=1`:

| | |
|---|---|
| feat A/B, 1024×768 | **0 differing px of 786 432**, **243 538 non-black on *each* side** |
| feat A/B, 1920×1080 | **0 of 2 073 600**, 506 936 non-black each side |
| both | capture files byte-identical; the 1024×768 pair reproduced **the same md5** on an independent relaunch, and read 0 again after `tagpu_vk.on` *and* `tagpu_feat.on` were cleared and re-armed — which frees and rebuilds the pipelines, the descriptor sets, the shared atlas image and every per-slot resource |
| scaffold A/B (§2.28's, the regression for the depth attachment) | **0 of 786 432**, **190 247 non-black a side — the same number §2.28 recorded** |
| fps A/B (§2.26's) | **0 of 786 432**, 89 ink px a side |
| constraint 4 | **0 of 630 784** in the world viewport against `6ad52e4` with `tagpu_vk.off` |

Nearly a third of the frame is feature ink, so this is not two blank frames agreeing — the failure
mode `tools/vk-ab.py` exists to name.

**It is not a second implementation of the pass:**

| | where it comes from |
|---|---|
| the vertices | `s_verts[B_SHADOW]` and `s_verts[B_BODY]` — the two arrays the GL gather filled and the GL upload took, handed over **exactly once** |
| the vertex layout | `TAGPU_FEAT_ATTRS` in `tagpu_feat.h`, **one literal both lanes build their vertex input from** (the GL VAO loops over it too) |
| the uniforms | the numbers the GL draw passed to `uGame`, `uZoom`, `uZoomC`, `uDepthScale`, `uRestored`, `uLit`, `uFog`, `uFogOrg`, `uFogDim` |
| the atlas texels | `tagpu_gaf.c`'s **CPU mirror** — written by the same `atlas_paint` that writes the GL texture, from the same `s_pad` rows, in the same call |
| the palette | `tagpu_pal_live()`, the buffer `s_palTex` is uploaded from. *Since G20c the world's colours come from `tagpu_pal_engine()` and the pass samples an RGBA8 base atlas instead (§2.3f, §2.88)* |
| the fog grid | a **copy of** the packet's grid, taken at publish time into a buffer the publishing module owns — the same bytes the GL lane hands `glTexImage2D(GL_RG8, …)`, and the pointer is not the packet's. It was the packet's until the G19e landing review (2026-09-15) pointed out that `tagpu_packet_frame_end()` ends that pointer's declared lifetime *before* `render_ogl.c` runs the Vulkan lane |
| the fog shade LUT | `tagpu_native_foglut()` — the 256 bytes last uploaded to `s_fogLutTex`, identity fallback included, published rather than rebuilt. *Deleted with the rest of the 8bpp path in G20d: the grey band is `(R+G+B)/3` in the shader (§2.88)* |
| the shader | `inc/spirv/tagpu_feat.spv.h`, generated from the GL string by §2.25 |

#### The depth answer, built

§2.28 wrote the rule down and this landing is where it became code. Two pieces:

* **The seam grew a depth attachment** (`tagpu_vk.c`): one image **per swapchain image** — `nimg`
  frames are in flight and each has a framebuffer of its own, so one shared buffer would be
  written by two overlapping frames — `LOAD_OP_CLEAR` at **1.0** (what `glClear(GL_DEPTH_BUFFER_BIT)`
  uses on the GL side, in the same place: the top of the world FBO), `STORE_OP_DONT_CARE`,
  `UNDEFINED` in. Both subpass dependencies grew their depth halves
  (`LATE_FRAGMENT_TESTS`/`DEPTH_STENCIL_ATTACHMENT_WRITE` →
  `EARLY_FRAGMENT_TESTS`/read+write).
* **The format is 24-bit fixed point or there is none.** [Replaced by G21a, §2.91: float with
  stencil first, 24-bit with stencil second, and the two test alike.] The GL lane's world FBO is
  `GL_DEPTH24_STENCIL8`, so a `D32_SFLOAT` attachment would settle a z-fight the other way in
  exactly the cases that are too close to call — which are the cases a 0-px comparison is made of.
  `vk_depth_format()` asks for `D24_UNORM_S8_UINT` and then `X8_D24_UNORM_PACK32`, and a device
  offering neither gets **no depth attachment at all**: the passes that do not test go on working
  and the ones that do refuse to arm and say so. On the reference setup it binds
  `D24_UNORM_S8_UINT` (the log prints `depth format 129`).
* **The range is the viewport's**: `minDepth = 0.5`, `maxDepth = 1.0`, which maps clip z ∈ [0, 1]
  onto exactly GL's `(z+1)/2`. Never a shader edit — an edited shader would disagree with the twin
  that is its oracle.

**Every pipeline in the lane now declares a depth state**, because `pDepthStencilState` may not be
null in a subpass that has a depth attachment. `tagpu_vk_fps.c` and `tagpu_vk_scaffold.c` declare
one with test and write **off**, which is what their GL twins do — and their own A/Bs re-measuring
0 px is what says the attachment cost them nothing.

**Two depth-write modes, so two pipelines.** The GL twin draws shadows under `glDepthMask(GL_FALSE)`
— they are ground decals and must occlude nothing — and bodies with it true. Two pipelines off one
layout needs no extension; `VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE` is Vulkan 1.3 or an extension and
would buy one object.

#### The scissor is the one number that is not the same on both sides

The native pass clips this draw to the world viewport, and a scissor is expressed in **framebuffer**
coordinates, which the two APIs disagree about. In the GL world FBO, framebuffer row 0 is clip-space
y = −1; with the negative viewport height this pass used until 2026-09-17, row 0 of the Vulkan image
was clip-space y = **+1**. The two images were stored the same way round — which is exactly what let
the bytes be compared at all — and a *rectangle* in one was the vertical mirror of the same
rectangle in the other:

    scissor.offset.y = H − (vpT + vh)

§2.28's "the scissor is NOT flipped" is true of a **full-frame** scissor and of a pass drawn into
the default framebuffer; this is neither, and the two statements do not conflict. The rect is also
scaled by the attachment's extent over the game frame's, which is the same division the vertex
shader does by `uGame` and reduces to the identity at the sizes an A/B runs at.

**And it travels with its ENABLE, not just its rect.** `tagpu_native_scissor_on()` reports whether
the clip is actually on (the native pass enables it only when it resolved `glScissor`). A Vulkan
lane that clipped while GL did not would differ in every feature the gather's margin reaches past
the viewport, which on a forest map is a wide band down both edges.

**[CORRECTED BY LANDING 5b, 2026-09-17 — the paragraph below was true of the comparison and hid a
defect in the lane.]** It read: *"Both halves of this A/B are upside-down pictures of the world, and
that is correct: the GL twin draws into the world FBO, whose clip-space +1 is the bottom of the
screen (the composite quad turns it over). They are upside down identically, which is the only thing
the comparison asks."* Every clause is true. What none of it covers is that the Vulkan lane has **no
composite quad** — the ported passes draw straight into the swapchain image — so making the Vulkan
picture match the GL *FBO* is what made Route D present the world upside down. The scissor above is
therefore no longer mirrored, this pass takes a **positive** viewport height, and the GL half of the
capture is written top-down (`TAGPU_ABSHOT_TOPDOWN`). §2.40 has the whole of it.

#### The atlas mirror — the mechanism the remaining world passes need

Every world pass samples an atlas, and no atlas in this tree keeps a CPU copy of its texels: the
decoder uploads and frees. So the first of them had to answer how a second backend gets those bytes.

> **`TAGPU_GAFATLAS.mirror`** — `dim × dim` bytes written by the same `atlas_paint` that writes the
> GL texture, from the same `s_pad` rows, in the same call. Not a second decode of the art: the very
> rows the `glTexSubImage2D` above it hands GL.

**It is correct from the instant it exists**, which is the part worth the paragraph. A mirror
allocated after the atlas has painted frames would hold zeros where those frames are, and a backend
uploading it would draw black trees for the rest of the session — silently, because nothing in the
atlas is wrong. So `tagpu_gaf_atlas_mirror` marks every painted entry **reserved** (`ok` 0,
`resv` 1), the state a repack leaves an entry in: the rect stays where it is and the next
`tagpu_gaf_atlas_get` paints it again, into GL and the mirror together. And because `atlas_get`
paints a reserved entry *before it returns it*, **every UV a vertex carries addresses texels that
are in the mirror** — the request is made on the GL twin's arm poll, which runs before the gather,
so the first frame that has a mirror is already correct. Entries nothing draws may stay stale in it;
nothing samples them, in either lane. Measured on `feat-forest`: `atlas mirror armed, 4096 KB — 13
painted frame(s) re-decode on their next use`.

It is asked for **on the 30-frame arm poll and not per frame**, because `tagpu_vk_armed()` is two
file-attribute queries and a pass that asked every frame would make them on every frame of ordinary
play, where the answer is no and stays no. 4 MB, so it is paid for only while the Vulkan lane is
armed, and once armed it stays for the process's life (there is no atlas destructor here).

#### The uploads: §2.28's cheaper design, taken up

§2.28 established one image and one staging buffer **per frame slot** on a one-line invariant, and
named the cheaper alternative — one image shared, behind a write-after-read barrier — saying to
reach for it when a pass needed the memory back. **This is that pass**, and the split is by size:

| | |
|---|---|
| **the atlas** | 2048² R8 = **4 MiB**. Per-slot it would be 16 MiB of device-local on a four-image swapchain, in a 32-bit address space whose largest free block is the number this phase spends its budget measuring. So it is **one image**, and the barrier at the top of an upload — `FRAGMENT_SHADER`/`SHADER_READ` → `TRANSFER`/`TRANSFER_WRITE` — orders the copy after every earlier frame's sampling. Legal across submits because a barrier's first synchronisation scope includes everything submitted to the queue before it, and a write-after-read hazard needs only an execution dependency; the access masks are for the layout transition, which is a write. Uploaded only when the mirror's serial says the bytes moved, and only the rows the shelf packer has used (`shelfY + shelfH`, published as `atlasRows`) |
| **the palette, the fog LUT, the fog grid** | 1 KiB, 256 B and 1 440 B at the measured fixture's 30×24 grid. **Per-slot**, because at that size §2.28's one-line invariant is worth more than the memory — and a dimension change (the grid's, whenever the view walks far enough) is then free: the slot is rebuilt at the moment we are handed it, which is the moment we own it. One staging buffer carries all three, and it is rebuilt **with** the fog image so the buffer and the image it feeds cannot disagree about the size |
| **the vertices** | per-slot, doubling from 64 KiB, never shrunk while the pass is drawing |

**The atlas's staging buffer is per-slot either way and cannot be anything else**: a barrier orders
GPU work and the hazard there is a CPU write. It is allocated on the frame that uploads and **given
back at that slot's next `prepare`** on which no upload is due — the same fence, one turn of the
slots later — so a settled scene holds none of it and a map still filling its atlas holds at most
one per slot while it does. *(The file header claimed this before it was true; §2.28's own lesson
— "a header claim is a claim" — is why it now is.)*

**And nothing is kept once there is nothing to draw**, exactly as §2.28's scaffold: a frame the GL
twin handed nothing over gives that slot back.

#### The A/B's oracle needed two more things, and both are in `tagpu_abshot.c`

* **`TAGPU_ABSHOT_DEPTH` — the depth buffer is cleared too.** For a pass that depth-tests the colour
  clear alone is half an oracle: the GL twin would test against what the passes *before* it left in
  the depth buffer (the terrain) while the Vulkan lane's render pass starts from a cleared one. The
  depth **write mask** is forced on for the clear and put straight back, because
  `glClear(GL_DEPTH_BUFFER_BIT)` is masked by it.
* **`TAGPU_ABSHOT_SCISSOR` — the scissor goes back before the pass draws.** The clear stays
  unscissored (a scissor left on by an earlier pass would black a rectangle instead of the frame),
  but a world pass clipped to the viewport *is* the pass and an unclipped one is something else.

Both are opt-in, so `tagpu_fps.c` and `tagpu_scaffold.c` measure exactly what they measured before.

**`ss` must be 1 for this A/B and the pass says so rather than writing a mismatched pair.** The GL
capture is the world FBO's viewport, `gw*ss × gh*ss`, and the Vulkan one is the window's client
rect; at `ss` 2 they differ by a factor of two and `tools/vk-ab.py` would refuse the pair after the
fact. `tacli arm <i> ss.off` is the lever.

#### What it does not do

**Classic++'s restored atlas is not mirrored**, so when the GL twin reports `uRestored` 1 this pass
draws **nothing** and says so once. Drawing with `uRestored` 0 instead would be a different picture
from the twin's and the A/B would report it as a rasteriser difference, which is the one answer an
oracle must never give. The twin is `tagpu_restoreglsl.c`'s RGBA8 surface; a mirror of it is the
same mechanism as the R8 one and is a later landing's. A `tacli` instance opts out of the play
defaults, so an A/B run does not meet this; a `--defaults` instance does.

**NOT COVERED.** No Vulkan **validation layer** ran — none is installed in the wine prefixes — so
the barriers, the stage masks and the layout transitions are argued from the specification and from
a correct picture. Also not covered: any resolution but 1024×768 and 1920×1080, any device but the
4070, Windows, `ss` 2, Classic++, and an **in-process map change** (every measurement here filled
the atlas from empty after a relaunch, so `tagpu_gaf_atlas_forget` and a repack have not been
watched feeding the mirror).

#### Constraint 4, measured — and the fixture trap is not the one on record

**0 differing pixels of 630 784** in the world viewport (`vp=(128,32,896,704)`), this landing's DLL
against `6ad52e4`'s, both with `tagpu_vk.off`, on `selbox-facings` at 1024×768, the full arm set,
pointer parked in the side panel, sim paused. The **cross-launch floor was measured first and read
0**: two launches of the landing's own binary differ by 0 px, and a third pairing (the base's launch
against a second launch of the landing's) also reads 0. §2.28's two-state 71-px sliver at
x 1017..1023 did not appear in any of the five launches taken here — it is a real artefact, not a
constant one, which is the whole reason the floor is re-measured every time rather than quoted.

**The trap that did bite is a different one, and §2.28 has it half right.** The engine's "forces
have been obliterated" chat lines expire on **TICKS, not on wall-clock seconds**, so "wait 35 s
after the load" is only true of a game that is *running*. Pausing with `tab` immediately after
`scenario load` freezes them on screen indefinitely: the first pair taken here read **8 282
differing px in a bbox of x 138..430, y 52..106** — the chat block — purely because one instance had
been paused at tick 155 and the other at tick 433. Let the game run until the lines go (about 60 s
at speed 10), *then* park the pointer and pause.

### 2.30 The terrain pass, drawn by Vulkan (`tagpu_vk_terr.c`, `tagpu_terr.c`'s two mirrors) — Phase G, G19e

The **third world pass**, the first **instanced** one, and the one the gate names an exact
oracle for. It is also the first whose shared textures **change size while the lane is up**,
which is the lifetime question §2.28 named and §2.29 did not have to answer.

**No engine address, and no new one was read to build it** — the pass takes everything through
`tagpu_terr_handover`, so there is nothing for [exe reverse engineering](../exe-reverse-engineering.html)
in this landing and nothing for the hook map above. `tagpu_vk_terr.c` is not on
`thread-split.allow` and may never need to be (Phase G standing constraint 1); the list is
**unchanged** at the 34 entries `thread-split-check.sh` reports.

**MEASURED 2026-09-15** under system wine on the reference setup's 4070, `ss=1` — and then
**RE-MEASURED IN FULL, TWICE**, on 2026-09-15: once on the binary the landing review's rework
produced, and again on the binary the **re**-review's fixes produced (md5 `7f50fa3e…`), which is
the one these figures are from and the one that would land. Both reworks touch the frame loop and
one of them touches the A/B harness itself, so a table taken on an earlier binary is a table about
a different program — that is the gate this landing failed the first time. **That md5 names a build, not a tree:**
the link is not byte-reproducible — two builds of the *identical* tree differ in exactly three bytes
(measured 2026-09-15: the COFF `TimeDateStamp` at `0x88`, the optional header's `CheckSum` at
`0xD8`, and the debug directory's copy of the stamp at `0x129804`), so re-hashing a rebuild can
never tell you whether you are on the measured binary. What can is that the *tree* has not moved —
`git status` clean at the commit the table cites — and that is the check to run before trusting a
figure here. **Every figure below
reproduced on every binary**, ink counts included; the only one that moves is the readout's, and it
moves by construction (see its row):

| | |
|---|---|
| terr A/B, 1024×768, `feat-forest` (Two Continents) | **0 differing px of 786 432**, **630 719 non-black on *each* side** |
| terr A/B, 1920×1080, same | **0 of 2 073 600**, 1 820 568 non-black each side |
| terr A/B, 1024×768, **Anteer Strait**, reached by an **in-process level cycle** | **0 of 786 432**, **630 784 non-black each side — the entire viewport** |
| all three | capture files byte-identical; a pair read 0 again after `tagpu_vk.on` *and* `tagpu_terr.on` were cleared and re-armed, which frees and rebuilds the pipeline, the descriptor sets, both shared images and every per-slot resource. That cycle has now been run on **both maps across the three binaries** — Anteer Strait (630 784 ink a side) on the rework, Two Continents (630 719) on the final one — and reads 0 every time. **It exercises `_down`'s non-owed branch and only that one**: with no refusal outstanding the pass must come back `ST_UNBUILT`, and it does. It says nothing about the owed branch, where the re-review found a real defect this run could not have caught (below) |
| feat A/B (§2.29's, the regression) | **0 of 786 432**, **243 538 non-black a side — §2.29's own number** |
| scaffold A/B (§2.28's) | **0 of 786 432**, **190 247 a side — §2.28's own number** (`tall=151` on the fixture) |
| fps A/B (§2.26's) | **0 of 786 432**. The ink count has read **88, 89, 92 and 94** across passing runs — **those pixels are the readout's own digits**, so the number follows the frame rate on screen and is not a property of the port. What the A/B asserts is that both halves carry the *same* digits, which is the 0. Do not treat this one as a regression figure; for the world passes the ink count *is* one, because there it is the scene |
| constraint 4 | **0 of 630 784** in the world viewport against **`afceba5` — `main`, i.e. the whole seven-commit landing rather than one pass's predecessor** — with `tagpu_vk.off` and the full pass set on both, over a **cross-launch floor of 0** measured by running the landing's own binary twice, and a within-run floor of 0 on all three instances |
| the owed teardown | **never fired**: 0 occurrences of `the seam tears it down` / `drained, tearing it down` / `WaitIdle refused` across every instance of every re-measurement. That is the expected reading — the path needs an allocation refusal — and it is worth grepping for, because a run that *does* print them was refused its resources for real |

The whole world viewport is terrain ink (630 784 px at 1024×768; the 65 black pixels are the
frame's own), so this is emphatically not two blank frames agreeing — the failure mode
`tools/vk-ab.py` exists to name.

**It is not a second implementation of the pass:**

| | where it comes from |
|---|---|
| the instances | `s_inst` — the four-short-per-cell array the GL gather filled and the GL upload took, handed over **exactly once** |
| the unit quad | `TAGPU_TERR_QUAD` in `tagpu_terr.h`, **one literal both lanes build their per-vertex buffer from** |
| the uniforms | the numbers the GL draw passed to `uGame`, `uZoom`, `uZoomC`, `uDepthScale`, `uEnc`, `uOrigin`, `uTile0`, `uTexel`, `uRestored`, `uHDim`, `uFog*`, `uLit`, `uLambert`, `uSun`, `uAmb`, `uNorm`, `uShadowOn` |
| the tile atlas | `tagpu_terr.c`'s **CPU mirror** — the very buffer `glTexImage2D` was handed, kept instead of freed |
| the height grid | the same, for the R8 grid `build_height` uploads |
| the palette | `tagpu_pal_live()`, the buffer `s_palTex` is uploaded from. *Since G20c the world's colours come from `tagpu_pal_engine()` and the pass samples an RGBA8 base atlas instead (§2.3f, §2.88)* |
| the fog grid | a **copy of** the packet's grid, taken at publish time into a buffer the publishing module owns — the same bytes the GL lane hands `glTexImage2D(GL_RG8, …)`, and the pointer is not the packet's. It was the packet's until the G19e landing review (2026-09-15) pointed out that `tagpu_packet_frame_end()` ends that pointer's declared lifetime *before* `render_ogl.c` runs the Vulkan lane |
| the fog shade LUT | `tagpu_native_foglut()` — the 256 bytes last uploaded to `s_fogLutTex`. *Deleted in G20d (§2.88)* |
| the shader | `inc/spirv/tagpu_terr.spv.h`, generated from the GL strings by §2.25 — a 5840-word FS with a 192-byte uniform block and eight samplers, and a 765-word VS with a 64-byte one |

#### Instancing, and the one trap in it

The GL twin draws **six vertices and one instance per visible cell** (`glDrawArraysInstanced`,
`glVertexAttribDivisor(1, 1)`), which is what lets a 4K view at the zoom floor fit in a
megabyte. In Vulkan that is a second `VkVertexInputBindingDescription` at
`VK_VERTEX_INPUT_RATE_INSTANCE` and `vkCmdDraw`'s `instanceCount` — small, and new to this lane.

**The trap is the format.** The cell record is four **unnormalised `GL_SHORT`s** read into a
`vec4` attribute, so GL's fixed-function conversion is "the integer, as a float". The Vulkan
format that does that is **`R16G16B16A16_SSCALED`**; `_SINT` would require the shader's
attribute to be an `ivec4` and would read as garbage against a `vec4`. SSCALED is not a format
a driver must support as a vertex buffer, so `build_pipeline` **asks** for
`VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT` and the pass stays down if the answer is no — naming the
fallback (widening the record to four floats on the CPU, where every field is a small integer
and exact) rather than guessing. The 4070 takes it.

#### The texels, and why this pass needed no mirror mechanism

§2.29's GAF atlas is painted **incrementally**, which is why it needed a mirror that is correct
from the instant it exists. Terrain's two big textures are built **whole**, once per map, out of
a buffer that was freed three lines later — so the whole of the answer is *do not free it*:

> **`s_atlasMirror` and `s_hMirror` ARE the buffers** `glTexImage2D` was handed, kept rather
> than freed. Nothing writes either again — the tile set is built by `LoadMap` and never
> changes — so they cannot drift from the textures.

The flag that asks for them (`s_mirrorWant`, set on the GL twin's 30-frame arm poll when
`tagpu_vk_armed()`) has one job the GAF mirror's did not: **force one rebuild when the lane arms
after the texture was built.** `ensure_atlas` and `ensure_height` both early-return on an
identity test, and the extra term in each is what makes them fall through *exactly once* — the
only way to obtain a mirror for a texture that already exists is to build it again. Measured:
on an instance armed before launch the poll runs before the gather, so the **first** build
already retains, and the log shows one `terr: atlas built …` line and no
`no CPU mirror yet` refusal.

**What they cost, and it is per map, not per frame:** Two Continents' atlas is 2176×2720 for
5062 tiles = **5.64 MB**, its height grid 672×800 = **525 KB**; Anteer Strait's are 2176×3774
for 7051 tiles = **7.83 MB** and 578×584 = **330 KB**. Paid for only while the Vulkan lane is
armed, and once asked for, kept for the process's life — un-asking would buy back memory that a
re-arm spends again.

**The hills mesh is not mirrored and does not need to be.** `build_hills` is the heightfield
*caster* for `tagpu_shadow.c`, a VBO/IBO the terrain fragment shader never samples.

#### Two shared images whose dimensions change — the lifetime §2.28 named

Per-slot, the atlas would be eight copies of 5.6–7.8 MB in a 32-bit address space whose largest
free block is the number this phase spends its budget measuring. So both big images are
**one image each**, uploaded when the mirror's serial says the bytes moved, behind §2.29's
write-after-read barrier.

Unlike §2.29's atlas, **their dimensions genuinely change**: `ATLAS_DIM` there is a compile-time
constant, while terrain's atlas is `ceil(tiles/64) × 34` texels tall and the height grid is the
map in 16-px cells. A shared image that has to be *replaced* cannot have its descriptor
rewritten under frames in flight. Two things make that safe by construction:

* **Every slot's samplers are rewritten during THAT SLOT'S OWN `prepare`** (`shared_bind`),
  which is the one instant the seam's fence proves nothing of ours is in flight for it. No write
  in the file ever touches a set another frame may be using — which is the property §2.29 got by
  doing its one such write before any set had ever been bound, and cannot be had that way here.
* **The replaced image is retired behind a slot bitmask, not a timer.** `pending` starts as
  every slot; a bit clears when that slot has been **visited** — at the top of its own
  `prepare`, unconditionally. It does **not** clear "when that slot's set has been rewritten":
  this page and the file header both said so until the G19e landing review (2026-09-15) and
  both were wrong, because the bit has to clear on the paths that return *without* binding or
  the retire stalls (see the paragraph below). Two facts bound the old image's last reference:
  no *submitted* command buffer can still name it through a cleared slot, because that slot's
  submit has completed; and no *future* one will, because `record` runs only when `prepare`
  returned 1 and every such `prepare` calls `shared_bind` first. So `pending == 0` means
  unreferenced, whatever the frame rate and whatever the driver. **That second fact was an
  argument spread over two functions and a seam in another file; since the review it also carries
  an assertion**: `shared_bind` records the views it wrote into each slot
  (`boundAtlas`/`boundHeight`) and `record` draws nothing unless they are still the live ones, so
  a slot left holding a retired view cannot sample it even if some future path reaches `record`
  without rebinding. **It is not, however, "the fact that licenses the destroy" — the `pending`
  bitmask still is, and this page said otherwise until the re-review.** `prepare` has exactly one
  `return 1` with `shared_bind` on the straight-line path before it, so the comparison is a
  tautology on every path that exists today and **can never fire**. It is a guard against a future
  `prepare` that returns 1 without binding, which is worth its two lines; it is not evidence.

**The accounting is done first and unconditionally**, at the top of `prepare`, and that is not a
detail: doing it inside `shared_bind` stalls for ever on the one path that matters — a resize
that cannot be applied because a retire is outstanding returns *before* binding, so the bit that
would end the retire would never clear. A second change arriving while one is outstanding draws
**nothing** for the frames it takes to clear rather than starting a second retire; at most
`slots` frames, and only for back-to-back map changes.

**MEASURED, and it is the honest half of this: that retire is NOT reached by the game as it
stands.** A map change is the only thing that moves either dimension, and it goes through a
shell transition that stops the GL render thread — which brings the whole Vulkan lane down and
back up, this pass included. The in-process cycle run here (Two Continents → `MAINMENU` →
Anteer Strait, no relaunch) logs `vk: render thread stopping - down` twice, rebuilds the atlas
at 2176×3774 and the height grid at 578×584, and brings the pass back with
`vk: terr: the Vulkan edition is up`. So the retire is **the guard that keeps the code correct
rather than a path the game reaches today** — and the code cannot be written without it, because
the dimensions are data and cannot be asserted away.

#### Pipeline state: depth writes, no blending, and the scissor again

* **Depth is §2.28's answer, used unchanged**: `VK_COMPARE_OP_LESS`, and **writes ON** — terrain
  is the frame's implicit far plane and everything above it is tested against what it wrote, so
  the pass refuses to arm when `d->dfmt` is `VK_FORMAT_UNDEFINED` rather than drawing untested.
  The viewport carries `minDepth 0.5 / maxDepth 1.0`, which is GL's `(z+1)/2` exactly. One
  pipeline, not §2.29's two: there is no shadow bucket here.
* **No blending.** The GL twin draws terrain *before* `glEnable(GL_BLEND)` (`tagpu_native.c`),
  and terrain is opaque with alpha 1 everywhere. §2.29's premultiplied pair is its own.
* **The scissor is §2.29's**, mirrored as `offset.y = H − (vpT + vh)` with its enable travelling
  with it. It matters more here: terrain covers the whole *gather* rect, which at zoom < 1
  reaches well past the viewport and over the side panel.
* **Both halves were upside-down pictures of the world — true until landing 5b and no longer**;
  the flip that made the Vulkan half match the GL *FBO* also made Route D present the world upside
  down, and both halves are now the right way up. §2.40.

#### What it does not do

**Two surfaces of the GL lane have no CPU mirror, and the pass refuses a frame that would need
either** rather than draw a different picture from its own oracle:

* **`uRestored` 1** — Classic++'s restored tile atlas (`tagpu_restoreglsl.c` writes it on the GPU
  and it is never read back), which is §2.29's refusal again.
* **`uShadowOn` 1** — the Classic++ cast-shadow depth map (`tagpu_shadow.c`), a GL depth texture.
  Its two samplers must still be **valid** for the set to be bound, and a `sampler2DShadow` needs
  a real depth image and a compare-enabled sampler — so they name a **1×1 `D16_UNORM` image**
  this file makes and clears to 1.0 with `vkCmdClearDepthStencilImage`. `uAtlasRGB` is a
  placeholder too and names the atlas's own view, exactly as §2.29's binding 42.
  **[SUPERSEDED BY §2.32, 2026-09-15.]** The map is drawn by `tagpu_vk_shadow.c` now and this
  pass samples it; the dummy is named only on a frame with no map, and the compare sampler is
  LINEAR rather than NEAREST because the twin's PCF is. What is left of this refusal is
  per-frame: a frame the shadow pass could not reproduce, which is every frame with a unit
  caster until the unit pass lands.

Both were Classic++ surfaces, so a `tacli` instance — which opts out of the play defaults — never
met either; a `--defaults` instance does.

**NOT COVERED.** No Vulkan **validation layer** ran — none is installed in the wine prefixes —
so the barriers, the stage masks and the layout transitions are argued from the specification and
from a correct picture. Also not covered: any resolution but 1024×768 and 1920×1080, any device
but the 4070, Windows, `ss` 2, Classic++ (both refusals above), **the retire firing** — the
one path in this file no run has watched execute, for the reason measured above — and, since the
landing review, **the owed teardown firing**, which is the same kind of gap for the same kind of
reason: it needs the device to refuse a slot its resources, and the 4070 with 247 MB of largest
free block does not. Both are guards that keep the code correct rather than paths the game
reaches today, and neither can be asserted away, because in both cases the trigger is data.

#### What the landing review changed, 2026-09-15

Two reviewers read `main...HEAD` independently at `high`, one on correctness and one on
synchronisation alone. **Both returned the same first finding**, and it was real. The fixes below
are on the branch, and **they have now been RUN**: every A/B, the regressions and constraint 4
were re-measured on the reworked binary and all of them reproduced — the table above is that
re-measurement, not the pre-rework one. The rework's own review — which `CLAUDE.md` asks for when a
fix changes the synchronisation *design* rather than patching it, and this one moves a teardown
across a submit boundary and adds a drain point to the seam's frame loop — **has since run, and it
found a blocker**: it is the next subsection, and the table above is the *third* measurement, taken
after its fixes.

**What the re-measurement does and does not cover.** It exercises every path the rework touched
*except the refusal itself*: the publish gate (every shipped frame in constraint 4 runs with the
lane down), the copied fog grid (every terrain frame), the `record` bound-views test (every
terrain frame, and it would show as a dropped draw over the whole viewport), the `_down`
state change (the clear-and-re-arm cycle), and the A/B write guard (every capture). **The owed
teardown itself is still unfired** — it needs the device to refuse a slot its resources, which is
the same reason §2.28's uncovered list has stood since the scaffold. So the drain path in
`tagpu_vk.c` is argued and reviewed, not run, and that is the honest statement of it.

* **A pass may no longer tear itself down mid-frame.** `prepare`'s refusal path — reached when
  the device will not give a slot its buffers or images, which is the 32-bit address-space
  pressure this phase exists to measure — used to call `tagpu_vk_*_down()` on the spot. That
  destroys the pipeline, the descriptor pool, the shared images and **every** slot's buffers,
  while the seam has waited on `fence[slot]` **alone**: every other slot's submit is still
  executing against them. Worse, the command buffer of the frame in hand has already had
  `shadow_ready`'s barrier and depth clear — and the atlas uploads — recorded into it, and
  `vk_present` submits it whether the pass draws or not. **It fires on the first refusal, not
  rarely.** The pass now stops drawing and raises `s_downOwed`; `tagpu_vk.c` checks
  `tagpu_vk_{terr,feat,scaffold}_down_owed()` at the top of the next frame — before `cb` is
  reset, so nothing names the objects yet — runs `vkDeviceWaitIdle`, and only then tears the
  pass down. An unresolved `vkDeviceWaitIdle` is **not** treated as idle: with no way to prove
  the device quiet the lane comes down instead. The `build()`-failure teardowns were left alone;
  they run with the pass `ST_UNBUILT`, so nothing of it is in flight.
* **The retire's stated invariant was wrong, and is now also a test** — see the bullet above.
* **The fog grid is copied at publish time** instead of handed over as a pointer into a
  frame-packet slot, whose declared lifetime (`tagpu_packet.h`) ends at
  `tagpu_packet_frame_end()` — which `render_ogl.c:1623` calls before it runs the Vulkan lane at
  `:1674`. It held only because the give-back happens at the next acquire; that is also where
  the `poison` lever fills the slot, so this was the one stale read that lever could not see.
* **The A/B no longer claims a Vulkan half on a GL half that was never written.**
  `tagpu_abshot_end` now returns whether the capture reached the disk, and the four callers set
  `s_abFrame` from it. `tagpu_fps.c` had this guard (`can`) on `main`; extracting the shared
  module dropped it, and the cost was `vk-ab.py` diffing a fresh Vulkan capture against a
  **stale** `_gl.ppm` from an earlier run and reporting a port failure.
* **Nothing is published on a shipped frame.** `terr_publish` and `feat_publish` ran their
  `memset` and ~40 stores on every frame with the lane down; they now return at once unless the
  module's mirror latch is set.
* **The cell count is bounded in the pass**, like the atlas, height and fog dimensions already
  were — it sizes both the instance buffer and `vkCmdDraw`'s `instanceCount`. The bound is the
  producer's own 24 MB clamp restated in this file's terms, deliberately **not** tighter: a pass
  that refused a cell count the GL twin drew would report a rasteriser difference over the whole
  viewport.

**Left unfixed, deliberately.** Arming the lane forces one height-grid rebuild
(`ensure_height`'s mirror term), and `build_height` zeroes `s_hW` and frees the mirror before it
starts — so if that one rebuild fails the *GL* lane draws unlit for up to 60 frames where it
previously kept a working texture. It needs `tagpu_vk.on`, and it needs a grid that was readable
a moment earlier to fail `ptr_ok`/`IsBadReadPtr`. Restructuring `build_height` to commit only on
success would break the "`s_hMirror` non-NULL and it is `s_hW × s_hH`" single-fact invariant that
function is written around, which is not worth doing inside a landing already carrying a
synchronisation rework.

#### What the RE-review changed, 2026-09-15

The rework above changes the synchronisation *design* rather than patching it — it moves a
teardown across a submit boundary and adds a drain point to the seam's frame loop — so `CLAUDE.md`
asks for a second review, and two more reviewers read `main...HEAD` at `high`, one on correctness
and one on synchronisation and object lifetime alone. **Both led with the same finding again**,
independently, exactly as the first pair did. Every finding below was verified against the code
before anything was changed.

* **THE BLOCKER, AND IT IS THE FIRST REVIEW'S BUG IN A SECOND PLACE.** The terrain hand-over could
  **outlive the frame that published it, and its pointers could be freed underneath it.**
  `s_pubHave` was cleared only inside `tagpu_terr_render`, which `tagpu_native.c` calls only when
  the gather returned cells — so every `terr_bail()` exit left the previous frame's hand-over
  standing, and `tagpu_terr_glreset` did not clear it either. Meanwhile `ensure_atlas` **frees**
  `s_atlasMirror` and `build_height` frees `s_hMirror` on a map change. The sequence is short and
  is not exotic: publish on a frame the lane is too young to consume, change level, the gather
  bails at `ptr_ok(tmap)` *during the load* — which is the same event that rebuilt the atlas — and
  the lane's next `prepare` `memcpy`s up to 5.9 MB **out of freed heap**. The non-crashing version
  of it is last frame's terrain drawn over this one, which `tagpu_terr.h` said was impossible.
  What makes it conclusive rather than theoretical is that **the codebase already knew the rule in
  two of its three places**: `tagpu_feat_glreset` clears the flag with a comment explaining this
  hazard, and `tagpu_scaffold_frame` clears unconditionally at the top of the frame. The terrain
  pass was the odd one out, and the asymmetry was sitting in the diff.
  **The fix is a bound, not an enumeration.** The hand-over now carries the frame it was published
  on and `tagpu_terr_handover`/`tagpu_feat_handover` refuse any other, so "these pointers are
  alive" is a property of the frame number rather than of which functions happened to run — a
  future `terr_bail` that forgets cannot resurrect the bug. The flag is *also* cleared on the bail
  paths and in `tagpu_terr_glreset`, so the flag tells the truth as well. The frame number reaches
  a pass as `TAGPU_VKPASS::frame`, from `tagpu_vk_frame`, from `render_ogl.c`'s own counter — the
  same number the GL lane stamped with earlier in that iteration.
* **A teardown nobody asked for could latch the pass refused for the life of the process.**
  `_down` read `s_downOwed` directly and ended at `ST_REFUSED` when it was set — but `vk_down` and
  `vk_resize` call `_down` for their own reasons, and `prepare` treats `ST_REFUSED` as terminal.
  So one transient refusal followed by a window drag (the acquire returns `OUT_OF_DATE`, the
  caller resizes before the seam's drain can run) killed the pass **permanently**, on a device
  that was then destroyed and replaced anyway. This was a regression the rework introduced: before
  it, `_down` always returned `ST_UNBUILT` and the pass recovered on the next lane cycle. The debt
  is now settled only by the seam's own `tagpu_vk_*_down_paid`, called after its `vkDeviceWaitIdle`;
  every other caller of `_down` leaves the debt standing and returns the pass `ST_UNBUILT`.
  **The re-arm measurement in the table above could not have caught this** — it exercises the
  non-owed branch — and the table now says so.
* **A failed fog copy published `fog` 1 with no grid.** `realloc` can refuse, which is precisely
  the address-space pressure this phase exists to measure, and the pass then sampled a 1×1 image
  while `uFogDim` carried the real dimensions: the GL twin draws correct fog, the port draws
  something else, in silence. Clearing `fog` instead would be just as silent a difference the
  other way, so the publisher now **publishes nothing at all** for that frame — the answer this
  file already gives for the restored atlas and the shadow map. Both publishers.
* **`tools/vk-ab.py` compared whatever files existed.** Existence is not freshness: every way a
  capture silently does not fire — the `.ab` lever not re-armed (`touch` on an existing file does
  nothing), the lane down, `ss != 1`, the seam's "N levers claimed this frame" refusal — leaves
  the previous run's PPMs on the disk, and the tool would print **that** run's verdict for the
  binary in front of you. **The whole of this landing's evidence is that number.** It now refuses
  a capture older than the `.ab` file that asked for it. (The re-measurements in the table were
  taken by a harness that deleted the PPMs before every capture, so a missing half would have been
  reported as missing; the tool no longer depends on the operator remembering that.)
* **`_down`'s early return** (`!dev || !vkDestroyBuffer`) discarded the verdict along with the
  debt. Neither reviewer could reach it and neither can I — `owed` implies the pass was `ST_READY`,
  which implies a live device — but it now latches `ST_REFUSED` for the same reason the path below
  it does.

**And four documentation claims the code disproves**, which is the half of a review this project
counts as findings rather than polish:

* **The `boundAtlas`/`boundHeight` test in `record` is inert — it can never fire**, because
  `prepare` has exactly one `return 1` and `shared_bind` is on the straight-line path before it.
  It is a guard against a future `prepare` that returns 1 without binding, and worth its two
  lines; it is **not** "the fact that licenses the destroy", which is what this page, the file
  header and `fecd146`'s own commit message all called it. The `pending` bitmask still is.
* §2.28's "loses 117 lines (501 → 384)" was never re-taken after `fecd146` added six lines:
  **111, 501 → 390**. The commit that re-measured this landing said "every number reproduced" and
  vouched for it without counting.
* The **ta-drive** skill told an operator to look for `depth format 0` on a device with no 24-bit
  depth. That line cannot be printed — the pass returns before it — and what does print is
  `the seam's render pass carries no depth attachment …`.
* The re-arm row of the table above read as though it validated the owed branch of `_down`. It
  validates the other one.

**What the reviewers checked and found clean** is worth recording too, because it is most of the
lane: the retire bitmask's invariant holds (bits clear under each slot's own fence; a second
resize is refused while one is outstanding; `slot_free` clears the bound record); no descriptor
set is written outside its own fenced `prepare`; the drain point really is before anything names a
pass's objects, and the debt is always settled and never twice; `CELL_MAX` equals the producer's
own clamp exactly and `HEIGHT_MAXDIM`/`FOG_MAXDIM` match theirs, none tighter; the GAF mirror
copies the rows `glTexSubImage2D` gets; and **no GL pixel moves on the unarmed path**. Nothing in
the rework rests on timing — the drain is a real `vkDeviceWaitIdle`, the retire is a bitmask under
fences, and no `IsBadReadPtr`, sleep or retry was added. The gaps were missing bounds, not weak
ones.

### 2.31 The effects pass, drawn by Vulkan (`tagpu_vk_fx.c`, `tagpu_fx.c`'s four buckets) — Phase G, G19e

The **fourth world pass**, the first that needs **more than one pipeline for one shader**, and
the first whose parity is not closed by construction — because it is the first that draws
**lines**.

**No engine address, and no new one was read to build it** — the pass takes everything through
`tagpu_fx_handover`, so there is nothing for [exe reverse engineering](../exe-reverse-engineering.html)
in this landing and nothing for the hook map above. `tagpu_vk_fx.c` is not on
`thread-split.allow` and may never need to be (Phase G standing constraint 1); the list is
**unchanged** at the 34 entries `thread-split-check.sh` reports.

**MEASURED 2026-09-15** under system wine on the reference setup's 4070, `ss=1`, 1024×768, on the
binary this landing builds and with the tree clean at that commit — which is the check that
matters, because the link is not byte-reproducible (§2.30).

| | |
|---|---|
| **triangles only** (`fx.on=nolines`), `fx-mix`, three runs | **0 of 786 432** every run, **5704 / 4061 / 23 932** non-black each side |
| **all four buckets**, `fx-mix`, three runs | **0 / 2 / 0**, ink 5008 / 3970 / 5812 each side |
| **all four buckets**, `fx-lasers`, three runs | **3 / 3 / 2**, ink 897 / 900 / 2835 each side |
| **lines only**, `fx-lasers`, four runs | **3 / 0 / 0 / 6**, ink 100 / 50 / 26 / 152 each side |
| constraint 4 | **0 of 630 784** against `main` with `tagpu_vk.off`, over a floor measured at **0** — see below, because the floor is the part that needed care |

So **everything this pass draws as triangles is exact** — the weapon sprites, the explosion
flashes with their additive blending, the debris, all ten particle layers, the palette lookups,
the fog and the depth test, at **35 707 ink pixels a side** on the busiest frame measured. What is
not exact is the **line** bucket, and the rest of this section is why.

#### Three pipelines for one shader, and why that is new

The GL twin draws four buckets with one program and three pieces of fixed-function state
(`tagpu_fx.c`, the draw at the end of `tagpu_fx_render`):

| bucket | topology | blend | what it is |
|---|---|---|---|
| `B_UNDER` | `TRIANGLES` | `ONE / ONE_MINUS_SRC_ALPHA` | particle layers 0..6 — wake foam, feature smoke, trail puffs, nanolathe |
| `B_LINES` | **`LINES`** | the same | lasers (RenderType 0) and lightning (RenderType 7) |
| `B_FLASH` | `TRIANGLES` | **`ONE / ONE`, additive** | the LHT explosion flash |
| `B_SPRITES` | `TRIANGLES` | back to the first | weapon sprites, explosions, particle layers 7..9 |

Topology and blend are both baked into a `VkPipeline`, so that is **three objects off one
layout** — the feature pass needed two for its depth-write modes and this is the same shape one
step further. `VK_EXT_extended_dynamic_state3` would make the blend dynamic and buy exactly one
object; it is not worth an extension. Depth is **tested and never written** for all three: the
native pass leaves `GL_LESS` and `GL_DEPTH_TEST` standing around this draw and the twin brackets
itself in `glDepthMask(GL_FALSE)`, so effects occlude nothing and are occluded by everything drawn
before them.

#### The line rule, found twice — one fixed and one open

**FIXED, and it is a named difference with a named fix.** Vulkan's default
`lineRasterizationMode` drew a **strict superset** of the twin: all 126 of its pixels plus
**exactly one extra fragment at the END of each segment** (4 px on the fixture, GL 126 ink against
Vulkan 130, with *no* GL-only pixel anywhere). GL's non-antialiased lines follow the **diamond-exit
rule** and Vulkan's default mode does not; `VK_LINE_RASTERIZATION_MODE_BRESENHAM` matched it on
this device — but it is not a guarantee: the specification lets a Bresenham line deviate from the
ideal fragments by up to one unit, so two conformant devices can differ (G21b, §2.93, replaced
every line primitive with an integer test for that reason). The seam
now asks for `VK_EXT_line_rasterization` and enables **`bresenhamLines`**, publishing it to passes
as `TAGPU_VKPASS::lineok`, the same shape as `flipok`. A pass with line vertices and no `lineok`
refuses the frame. That took 4 px to 0.

**The test for it is a QUERY, and this file said otherwise until the landing review.** It claimed
that enabling the feature made `vkCreateDevice` the test, because a device that lacks a requested
feature must fail with `VK_ERROR_FEATURE_NOT_PRESENT`. That does not hold here, and **both**
reviewers found it independently: this instance is Vulkan 1.0 and did not enable
`VK_KHR_get_physical_device_properties2`, which `VK_EXT_line_rasterization` depends on — so an ICD
that does not consider the extension properly enabled is entitled to **ignore the unrecognised
`pNext` struct and return `VK_SUCCESS`**, which is indistinguishable from having enabled it. The
pass would then build its line pipeline with `BRESENHAM` chained on a device where the feature is
off: undefined behaviour rather than an error, and in practice the default mode — the four-pixel
superset this was added to remove, back again with nothing in the log. The seam now **asks the
instance for that dependency when the loader offers it, and asks the DEVICE for the feature bit**
through `vkGetPhysicalDeviceFeatures2KHR` before setting `lineok` at all; no query, no lines.

**OPEN, AND IT IS STRUCTURAL: THE Y FLIP AND EXACT LINE RASTERISATION ARE IN TENSION.** What is
left with Bresenham on both sides is a **one-row shift of a whole segment at a single column**,
deterministic to the pixel — the same 3 px on every run of the same geometry. The measured case:
one laser bolt, **x 553..602 (dx 49) by y 410..430 (dy 20)**, two parallel lines of different
palette colours; **49 of its 50 columns are identical**, and at x 578 the GL pair sits at rows
{420, 421} and the Vulkan pair at {419, 420}.

The reason is the flip, and it is worth stating carefully because every remaining pass that draws
lines inherits it:

* A **triangle** is filled by a coverage test each pixel centre passes or fails on its own. That
  test is symmetric under reflection, so mirroring the raster grid mirrors the result exactly —
  which is why every world pass so far reads 0 and why the triangle buckets above do.
* A **line** has no inside. It is *walked*: step a column, round to the nearer row, step again.
  Where the exact line passes **exactly halfway** between two rows the walk needs a tie-break, and
  the rule — in both APIs — rounds toward larger `y` **in the space the rasteriser is working in**.
* The Vulkan lane rasterises the frame **mirrored** (the negative viewport height, which is how
  every ported shader keeps GL's clip convention without a source edit). Mirroring is exact: a
  pixel centre maps to a pixel centre and an endpoint to an endpoint, nothing lands between. But
  "round toward larger `y`" in mirrored space is "round toward **smaller** `y`" in the world. So at
  a tie, **and only at a tie**, the two lanes choose opposite pixels.

That is why it is rare (one tie in fifty columns on this bolt, none at all on the shorter ones)
and why it is perfectly reproducible when it happens.

**THE BAR IS ONE PIXEL PER TIED SEGMENT, NOT A FIXED THREE**, and the table above is what that
looks like. A segment that hits a tie loses one pixel and gains one: **2 px** for a bolt drawn as a
single line, **3 px** for one drawn as two parallel lines (the pixel between the lost and gained
rows also changes which of the bolt's two colours wins there), and they ADD — **6 px** was measured
on a frame with two tied bolts. So the honest statement of the bar is *a couple of pixels per laser
that happens to land halfway*, with **6 of 786 432 the worst seen** across every run taken here, and
not a constant. A frame full of lasers has more segments and therefore more chances to tie; nothing
measured here bounds it at six. **There is no fix inside the current
design**: pre-mirroring the line geometry, or flipping in the shader, would mean the Vulkan lane
draws from something other than what the GL lane drew, which is the one thing the oracle forbids.
**The owner's decision, 2026-09-15, is to ship it as a stated bar** — a couple of pixels per tied
laser, 6 of 786 432 at the worst measured, on bolts that move every frame — rather than refuse line
frames. It is the
first time this lane claims a bar where an exact oracle *does* exist, and it is recorded here
because the unit pass's wireframe bucket (`tagpu_posedraw.c`'s `TAGPU_PB_WIRE`) and the hi-res
path draw lines too and will inherit exactly this.

#### A three-byte format Vulkan does not have

The flash light table is `glTexImage2D(GL_RGB8, 32, 1, …)` on the GL side, and
**`VK_FORMAT_R8G8B8_UNORM` is optional and not supported for sampled images** on the device this
lane is measured on. The 96 bytes are therefore **expanded to 32 × 1 RGBA8** on the way into the
staging buffer, alpha 255; the shader reads `.rgb`, so the byte it gains is never looked at.
`build_sampler` asks for every format this pass samples **by name** and refuses the pass rather
than discover it later — a format the GL lane takes for granted and the Vulkan lane has to ask for
is exactly the kind of thing that is invisible until a picture is wrong.

#### It is not a second implementation of the pass

| | where it comes from |
|---|---|
| the vertices | `s_verts[4]` — the four bucket arrays the GL gather filled and the GL upload took, handed over **exactly once**, concatenated in the twin's own draw order |
| the vertex layout | `TAGPU_FX_ATTRS` in `tagpu_fx.h`, **one table both lanes build from** — the GL VAO and the `VkVertexInputAttributeDescription` array |
| the uniforms | the numbers the GL draw passed to `uGame`, `uZoom`, `uZoomC`, `uDepthScale`, `uFog`, `uFogOrg`, `uFogDim`, `uScafP`, `uSS`, `uZoomF`, `uZoomCF`, `uRestored` |
| the sprite atlas | `tagpu_gaf.c`'s **CPU mirror** of the fx atlas, §2.29's mechanism, asked for on the arm beat only while the Vulkan lane is armed |
| the palette | `tagpu_pal_live()`, the buffer `s_palTex` is uploaded from. *Since G20c the effects take `tagpu_pal_engine()` and sample an RGBA8 base atlas that carries the flash level in its alpha (§2.88)* |
| the flash light table | `s_lhtRGB`, the very buffer `glTexImage2D` was handed, moved to file scope for this |
| the fog grid | a **copy of** the packet's grid, taken at publish time into a buffer the publishing module owns — never the packet's pointer, whose declared lifetime ends at `tagpu_packet_frame_end()` |
| the fog shade LUT | `tagpu_native_foglut()`. *Deleted in G20d (§2.88)* |
| the shader | `inc/spirv/tagpu_fx.spv.h`, generated from the GL strings by §2.25 |

#### What it does not do

* **THE SCAFFOLD TEST.** `uScafOn` is 1 for the `B_UNDER` draw alone when the G12a scene-depth
  scaffold is armed, and the fragment shader then samples `uScaf` — **which is another pass's
  texture**. `tagpu_vk_scaffold.c` holds those texels in an image it owns privately and
  `tagpu_vk_scaffold.h` exposes no view; sharing one image between two passes is a mechanism with
  an ordering contract of its own and this landing does not build it. A frame whose twin had the
  test on is **refused**, said once. The scaffold is a debug overlay and is not in the default arm
  set, so the measured configuration is unaffected — but **the unit pass and the hi-res path
  sample the same texture** (`tagpu_native.c:3834, 3994, 4245, 4303`), so whichever landing ports
  those has to answer it, and the answer should be chosen once for all three.
* **Classic++'s restored atlas**, as in every world pass: no CPU mirror, so a frame whose twin
  reported `uRestored` 1 draws nothing and says so once.
* **The effects MODELS.** RenderType 1/3/6 projectiles are emitted as 3DO nodes and drawn by
  `tagpu_native.c`'s unit pipeline, not by `tagpu_fx.c`'s own program — so they are the unit
  pass's to port, and the A/B's clear erases them from the GL half exactly as it erases the
  terrain.
* **`ss` 2.** `glLineWidth(ss)` makes the twin's lasers two pixels wide, and a Vulkan `lineWidth`
  other than 1.0 needs the `wideLines` device feature, which is the seam's to enable and it does
  not. A frame with line vertices at `ss != 1` is refused, naming the cause. (The A/B needs `ss=1`
  anyway: the GL capture is the supersampled FBO and the Vulkan one is the client rect.)
* **The validation layer**, still — none is installed in the wine prefixes, so the barriers and
  stage masks are argued from the specification and a correct picture.
* **The owed teardown**, still unfired on any instance, as in every pass since the rework.

#### Constraint 4, measured — and the floor artefact is the PAUSED banner, blinking

**0 differing pixels of 630 784** in the world viewport (`vp=(128,32,896x704)`), this landing's
DLL against `main`'s (`e39b762`), both with `tagpu_vk.off` and the full arm set, on
`selbox-facings` at 1024×768, pointer parked in the side panel, sim paused — **on two independent
pairs**, and **0 for every other pairing taken**, including two separate relaunches of the same
binary. **Re-measured on the binary the landing review's fixes produced and still 0**, which is the
whole table's rule here: those fixes changed device creation, so every figure above was taken
again on the binary that would land rather than carried over.

**The artefact that looked like a floor is the engine's own `PAUSED` banner, and it BLINKS.** Two
paused instances captured at different moments in that blink differ by the whole banner:
**2 747 px** on one pair, **2 933 px** on another, in a band at **x 502..636, y 372..400** — which
is exactly where the word sits at this resolution. Every one of those pixels is inside it and
**none is outside**:

| pair | differing px | outside the banner |
|---|---|---|
| landing vs `main`, pair 1 | 0 | **0** |
| landing vs `main`, pair 2 | 2 747 | **0** |
| landing vs landing (same binary, two instances) | 0 | **0** |
| landing vs itself, separately relaunched | 2 933 | **0** |
| `main` vs landing, separately relaunched | 2 933 | **0** |

So the GL lane did not move, and the way to see that is to **exclude the banner rect or catch both
halves in the same blink phase** — not to load and pause the two instances together, which is what
this section said until the crop was actually looked at. Loading together changes nothing: the
blink is free-running, and a simultaneous pair lands in phase or out of it by luck. *(The first
pair here read 0 and the inference drawn from it — tick skew, fixed by pausing together — was
wrong; cropping the 180×60 region and rendering it showed the word `PAUSED` in one capture and
bare grass in the other. §2.28's rule to measure the floor every time is what surfaced it, and
looking at the picture rather than the number is what explained it.)*

This is a **different artefact from §2.28's two-state 71-px sliver** at x 1017..1023, y 236..277,
which is at the frame's right edge and is not the banner. Both are reasons the floor is measured
rather than quoted.

### 2.32 The cast-shadow depth map, drawn by Vulkan (`tagpu_vk_shadow.c`, `tagpu_terr.c`'s caster mirror) — Phase G, G19e

*Since 2026-09-23 the caster mirror is gone: `build_hills` and `tagpu_terr_hills_draw` were deleted,
so no module builds a heightfield caster and the pass has no producer (`tagpu_vk_shadow.h`,
`TAGPU_SHADOWHAND`). The pass itself is kept. What follows is the landing's record.*

The **fifth world pass**, and the first that **draws into something other than the frame**. Every
pass before it records into the seam's render pass and produces pixels; this one owns an offscreen
depth image, draws the casters into it from the light's point of view, and leaves it in
`SHADER_READ_ONLY_OPTIMAL` for the passes that sample it. `tagpu_vk_terr.c` stops refusing
`uShadowOn` frames and samples it, which closes an uncovered case §2.30 had on record.

**No engine address, and no new one was read to build it** — the pass takes everything through
`tagpu_shadow_handover`, so there is nothing for
[exe reverse engineering](../exe-reverse-engineering.html) in this landing and nothing for the hook
map above. The one engine word this work reads at all is `main+0x37F06` bit 2, the Shadows option
the GL twin already gates on, and it is already recorded there. `tagpu_vk_shadow.c` is not on
`thread-split.allow` and may never need to be (Phase G standing constraint 1); the list is
**unchanged** at the 34 entries `thread-split-check.sh` reports.

**MEASURED 2026-09-15** under system wine on the reference setup's 4070, `ss=1`, 1024×768, **on
the binary the landing review's fixes produced** — every figure below was taken twice, once before
the review and once after, and the second table is the one that stands. (The first run's ink counts
were a few hundred higher because the camera was a pinned one rather than the scenario's own; the
verdicts were the same.)

| | |
|---|---|
| **a heavily shadowed frame** — `static-terrain`, Classic++ `assets=0 shadows=1 terrainshadow=1 shadowsun=225,8`, where turning the map off moves **529 130** of the frame's pixels | **0 of 786 432**, **630 458** non-black each side, reproduced |
| the same, **`light=0`** (the lambert off, the map still on) | **0 of 786 432** |
| an **empty** map — `terrainshadow=0`, which is the Classic++ **default** | **2 of 786 432**, and they are the lambert's (below) |
| the default sun `225,40`, `terrainshadow=1` | **2 of 786 432**, same pixel every run, three runs |
| the control: `shadows=0` (no map at all) | **the same 2 px**, at the same pixel |
| the control: `light=0 shadows=0` | **0 of 786 432** |
| the **refusal** with one posed caster on screen | fires, says so once, the terrain pass stands down with it, and both recover to **0 px** (630 574 ink) when the caster leaves view |
| constraint 4 | **0 of 630 784** against `main` (`2e552d1`) with `tagpu_vk.off`, on **two** pairings, over a cross-launch floor measured at **0** |

**AND THE CASTER-MESH RETIRE HAS ACTUALLY FIRED**, which is the first time a retire in this lane
has: §2.30's shared-image one is still argued rather than run, because only a map change moves it
and that brings the whole lane down. This one is moved by a **live knob** — `terrainshadow` back to
0 gives the buffers back through the retire, and turning it on again logs a second
`caster mesh uploaded`. The **free** is what the next frame proves, not the upload: `mesh_resize`
returns −1 while `oldV` is still set, so a pass that drew 0 px with full ink after the cycle is a
pass whose `pending` bitmask reached 0 and whose `kill_buffer` ran.

So the depth map itself is **exact**: its geometry, its stored depth values, the 16 Poisson taps on
the raw depths, the blocker search, the bilinear PCF through the compare sampler and the
receiver-plane bias all reproduce the GL twin bit for bit over a frame two thirds of which is
shadow. The 1–2 px that remain are **not the shadow pass** — see "The lambert is 1 px, and it is
not this landing's" below.

#### It owns a second render target, and that is not a breach of the seam

Standing constraint 3 says surface, swapchain, acquire and present live in `tagpu_vk.c` and that
nothing else may know a window exists. **A render pass and a framebuffer over an image this file
allocated name no window**, so the second target lives in the pass rather than in the seam: the
seam still owns the one image that reaches a screen, and the pass would work unchanged against an
offscreen frame or another process's. That also keeps the seam's file from growing a second
personality every time a pass wants somewhere to draw.

What it costs is that the pass records a **whole render pass inside `prepare`** — begin, draw, end
— which is legal precisely because `prepare` is the hook the seam calls *outside* its own
`vkCmdBeginRenderPass`, and render passes may not nest. The two-phase contract was written for
texture uploads (a transfer may not be recorded inside a render pass); a second render pass turns
out to need exactly the same hook for exactly the same reason.

So the pass has a `prepare` and **no `record`**, and the seam does **not** add it to the `ndraw` /
`nclaim` counts. Those two exist to catch two passes contaminating one A/B capture; this one puts
no pixel in the frame, so counting it would refuse every capture taken while Classic++ shadows are
on — which is the configuration the shadow work is measured in.

#### GL's clip-space z range, and this is the pass that needed it

§2.29 item 1 settled the depth question for every pass that came after it: GL maps clip z in
[-1, 1] onto the depth range and Vulkan takes [0, 1], so a viewport with `minDepth 0.5 /
maxDepth 1.0` reproduces GL's `(z+1)/2` exactly, values and precision included, and
`VK_EXT_depth_clip_control` "is the answer only if a shader is ever found writing a z below 0."

**The shadow matrix is that shader.** `tagpu_shadow.c`'s `mrow` builds an orthographic projection
that fills [-1, 1] **by construction** — `ndc = 2(a·W − lo)/s − 1` over the light-space extent —
so under Vulkan's own convention the near half of every caster is CLIPPED AWAY and the map is
wrong rather than merely offset. And the values matter as much as the geometry: `taShadowAt`
compares `p.z * 0.5 + 0.5`, computed in the consumer's own shader, against what is STORED in the
map, so the viewport transform has to be GL's arithmetic and the format has to be GL's
quantisation.

[Replaced by G21a, §2.91: the caster vertex stages remap z to `(z + w)/2` themselves, and the
extension, `zclipok` and the pipeline state below are gone.]
The seam therefore queries `depthClipControl` and publishes it as `TAGPU_VKPASS::zclipok`, in the
same shape as `flipok` and `lineok`, and the pass stands down without it. **The feature is
QUERIED through `vkGetPhysicalDeviceFeatures2KHR`, never inferred from a `vkCreateDevice` that
succeeded** — the effects review's rule (§2.31), applied on the first day rather than after a
reviewer found it — and the device-creation ladder gains its own rung: depth clip control is
dropped first because it costs one pass, then line rasterisation, then the flip, and each rung
rebuilds both the extension list and the `pNext` chain rather than unlinking one struct out of
the middle of it.

**Measured on the reference setup, 2026-09-15**, with `tools/vkprobe.c` extended to print every
extension: the 4070 offers `VK_EXT_depth_clip_control` from a 32-bit process under **system wine
9.0**, and the device answers `depthClipControl` true. Two numbers worth recording beside it,
because they are not the ones §2.24 has: this wine enumerates **186** device extensions where the
Proton 11 run recorded 247, and neither list carries the four ray-tracing ones at 32-bit.

#### There is no Y flip here, and that is not an omission

Every pass that is **presented and writes GL's window convention** flips — a negative viewport
height, `VK_KHR_maintenance1` — because the two APIs disagree about which row of a window is the
top. Since landing 5b that is `gui`, `fps` and `scaffold` only: the world passes write the engine's
screen-space y, which grows downward, so clip −1 is already the game frame's top row and they take a
**positive** height. This pass was right for a third reason and still is.

**This target is sampled.** In GL, clip y = −1 is window row 0, which is texel row 0, which is
v = 0. In Vulkan with a *positive* viewport height, clip y = −1 is framebuffer row 0, which is
texel row 0, which is v = 0. The two agree already, and flipping would put every shadow in the
wrong half of the map. **The flip is a property of presentation, not of Vulkan** — which is worth
stating plainly, because "every pass flips" was on its way to becoming a rule of this lane rather
than a consequence of what each pass draws into.

Cull is off on both sides (`tagpu_shadow.c` disables `GL_CULL_FACE`, so a caster's back faces
write depth too), so the winding a flip would also have inverted is not in play either.

#### One map per frame slot; the caster mesh is shared and takes the retire

The map is written every frame and sampled in the **same** frame by the consumers, so with frames
in flight one image would have frame N's writes racing frame N−1's reads. **One image per slot**
makes the seam's fence the whole argument, exactly as the scaffold's per-slot upload does — and it
means a resolution change (the zoom octave moves the map between 256 and 4096) is a rebuild of the
slot we are being handed, under its own fence, rather than a retire. At the default `shadowres`
2048 that is 16 MB a slot and 64 MB at the ceiling, none of it allocated until a map is drawn.

The **caster mesh** is the other way round: it is shared by every slot and its size is data, so it
takes §2.30's slot-bitmask retire unchanged — the old buffers are held until every slot has passed
through its own `prepare` once, and `pending == 0` is then "no submitted command buffer names them
and no future one will", by construction. The only difference from the terrain pass's is that a
buffer is named by the **command buffer** rather than through a descriptor set, which changes
nothing about the argument. The accounting is done **first** in `prepare`, before every early
return, because the path that returns early is exactly the path that must still clear its bit.

`mesh_resize` returns **three** values rather than two — 1 built, −1 a retire still clearing, 0 the
device refused — because a refusal that read as "a retire is clearing" would loop silently for
ever, which is the shape §2.30's own `shared_resize` guards against with its
"is there an image at all" test.

#### The texels: the mirror is the buffer, again

`tagpu_terr.c`'s `build_hills` builds the heightfield caster once per map — one world-space vertex
per 16-px grid point, two triangles per cell, indices ordered by cell row so the rows under the
light window are one contiguous range — and hands both arrays to `glBufferData`. Since this
landing it **keeps** them while the Vulkan lane is armed (`s_mirrorWant`, the same latch the tile
atlas and the height grid use) instead of freeing them, so the Vulkan pass draws the same vertices
and the same index order rather than a second evaluation of that arithmetic. On Town & Country
that is **291 600 vertices and 1 743 126 indices, 10 226 KB**, uploaded once on the serial.

`tagpu_terr_hills_draw` also reports **the index range it actually drew**, clamped, so the Vulkan
pass draws the same `firstIndex`/`indexCount` rather than re-deriving the row clamp — the port
rule ("the port must not re-derive the pass's inputs") applied to a draw range rather than to
texels.

#### Only the heightfield casts, and a map missing a caster is a different map

The GL map is drawn from **four** kinds of geometry: the native 3DO stream, the posed program's
depth twin, the replacement meshes, and the heightfield. Only the heightfield has a CPU mirror on
this side of the seam today; the other three are the **unit pass's** to port.

So `tagpu_shadow.c` **counts** the casters it drew that the hand-over carries no copy of, and a
non-zero count is a **refusal** rather than a best effort. An OVER-count is the safe direction: it
refuses a frame the lane could have drawn, where an under-count would draw a different picture from
its own oracle. Measured: one posed caster on screen refuses the map, the terrain pass stands down
with it and says so, and both come back to 0 px the moment the caster leaves view.

**The census covers all FOUR kinds, and it did not at first — both reviewers led with that,
independently.** The native stream counts itself inside `tagpu_shadow_unit`; `tagpu_native.c`
reports the posed and hi-res ones the same way both loops draw them (`!castSkip`); and **the
heightfield counts itself too**, because `tagpu_terr_hills_draw` returns 1 whenever it issued the
draw and fills its out-parameter only when the mirror is there. "Drew, no mirror" therefore arrived
at the hand-over as a zeroed struct — **byte-identical to "the hills did not draw"** — which this
pass reads as an empty map and reports as complete, so the terrain pass would sample an all-1.0 map
while the GL twin's held the whole heightfield, and the A/B would call that parity. It is
reachable: `build_hills`' out-of-memory exit returns before it touches `s_hMeshW`/`s_hMeshH` or the
GL buffers, so a same-grid mesh from an earlier build keeps drawing while `s_hMeshNoMirror` stops
`ensure_height` ever asking again. `tagpu_shadow_hills` now treats that as one more caster with no
copy, which is the refusal the design already had.

**An empty map is not refused.** With `terrainshadow` at its default 0 and no unit caster in view
the GL twin draws nothing into its depth texture either, and the clear at 1.0 **is** the map —
every receiver then finds no blocker and is lit. Refusing that would stand the consumers down over
a map this lane reproduces with a render pass and no draw call, and it is the Classic++ default
configuration. Measured at the same 1 px as the `shadows=0` control, i.e. exactly.

#### The consumer's compare sampler had to become the twin's

`tagpu_vk_terr.c`'s compare sampler existed only so that `uShadowCmp` was a valid descriptor
(§2.30) and was NEAREST, "which keeps it off `SAMPLED_IMAGE_FILTER_LINEAR` for a depth format".
Now that it is sampled for real it has to be **LINEAR**, because the GL twin's is
(`GL_COMPARE_REF_TO_TEXTURE` + `GL_LEQUAL` + MIN/MAG LINEAR) and the bilinear filtering is half of
what makes the penumbra smooth.

Linear filtering of a depth format is a **feature bit**, so it is asked of both formats the sampler
is ever used against — the map's and the 1×1 dummy's — and a device that will not filter either
keeps the NEAREST sampler and stands the pass down on a frame that would actually sample it. The
map's format is **exposed by the shadow pass** (`tagpu_vk_shadow_format`) rather than re-derived in
the consumer, so the two files agree by construction instead of by both happening to try the same
candidates in the same order. On the reference setup it is `VK_FORMAT_D32_SFLOAT`, with linear
filtering (§2.91).

**So the terrain pass has a third way to stand down**, beside the restored atlas and a map this
frame's shadow pass could not draw: a device that will not filter a depth format linearly. **And
that gate is deliberately stricter than the specification**, which the review raised and which is
written down rather than acted on: VUID-vkCmdDraw-magFilter-04553 conditions the `FILTER_LINEAR`
format feature on `compareEnable == VK_FALSE`, so a depth-*compare* sampler is arguably entitled to
LINEAR without it. The reference device offers the bit either way, so nothing is lost here; being
wrong in the other direction is undefined behaviour rather than a stand-down, and this lane still
has no validation layer to settle it with. Revisit it with one running.

#### The lambert is 1 px, and it is not this landing's

The 1–2 differing pixels in the table above are the **Classic++ lambert**, and the controls say so
outright: `light=0` reads 0 px with the map on, `light=1 shadows=0` reads the same 1 px at the same
pixel with no map at all, and the heavily shadowed frame — which moves that pixel's value off
whatever boundary it was sitting on — reads 0.

It is a **newly measured** property rather than a new one. §2.30 measured the terrain pass at 0 px
on `feat-forest`, which is not a Classic++ frame; with Classic++ on the pass refused every frame
(the restored atlas), so the lambert path had never been through an oracle. `assets=0` is what
opens it, and the answer is **one pixel of 786 432, one level**, deterministic for a given camera.
Both lanes run the same GLSL — one through the NVIDIA GLSL compiler, one through glslang's SPIR-V
and the NVIDIA SPIR-V path — and a single fragment landing on a quantisation boundary is the
expected shape of the difference. **Stated as a bar, like the line tie-break**, and it is the
terrain pass's rather than the shadow pass's.

#### The fixture, and why finding one took longer than writing the pass

`terrainshadow=1` "self-shadows the ground", which is why it defaults to 0 — and at the **default**
sun (`shadowsun=225,40`) that self-shadowing is nearly invisible. Measured on `static-terrain`
(Town & Country), nine camera positions across the map, shadows on against shadows off: **0 px at
seven of them, 38 px and 54 px at the other two**. A 0-px A/B taken there proves nothing about the
map, and the first one taken here did not.

What opens it is the sun's **elevation**, which is a live knob:

| `shadowsun=225,EL` | pixels the map changes, of 786 432 |
|---|---|
| 40 (the default) | ~0–54 |
| 25 | 645 |
| 15 | 8 042 |
| **8** | **517 270** |
| 4 | 535 034 |

So `shadowsun=225,8` is the fixture: two thirds of the frame is terrain shadow and every one of
those pixels depends on the map's contents. **Confirm the picture actually depends on the thing
under test before believing a 0** — the GL-on against GL-off diff is one command and it is what
turned a vacuous pass into a measurement.

#### Not covered

* **Every caster but the heightfield** — the units, the posed bodies and the replacement meshes,
  which is the unit pass's landing. On any fixture with a unit on screen this pass refuses, and
  the terrain pass refuses with it.
* **`ss` 2**, as for every world pass (the two captures would be different sizes).
* **Classic++ `assets=1`**, because the restored tile atlas still has no CPU mirror — the terrain
  pass refuses it and the restorer's five shaders are still not in the SPIR-V pipeline.
* **The owed-teardown path**, which has still never fired on any pass: it needs the device to
  refuse a slot its resources. The lane's oldest open hole, argued and reviewed rather than run.
* **The resolution change**, i.e. the per-slot rebuild when the zoom octave moves `res`. It needs
  `zoom.on` armed and a zoom-out past the octave boundary, and this landing's fixture is at 1×.
* ~~The caster-mesh retire~~ — **covered**, see the table: `terrainshadow` is a live knob, so the
  retire fires and completes without a map change. §2.30's shared-image retire is still the one
  nothing has watched execute.
* The validation layer, still, and no device but the 4070.

### 2.33 The unit pass, drawn by Vulkan (`tagpu_vk_unit.c`, `tagpu_posebake.c`'s two mirrors) — Phase G, G19e

The **sixth world pass and the last of G19e**, and the first that **both feeds and samples another
pass's target**: it puts the posed casters into the cast-shadow depth map §2.32 draws and then
samples the finished map in its own fragment shader. That is why it has four hooks where every
pass before it had two, and why three of them are called at three different points in the seam's
frame.

| hook | where | what |
|---|---|---|
| `upload` | before `tagpu_vk_shadow_prepare` | the vertex buffers this frame's types need, the pose blocks, the texels |
| `cast` | **inside the shadow pass's render pass** | the posed casters into the map |
| `prepare` | after the map is drawn | point this slot's set at the map and the overlay; decide whether to draw |
| `record` | inside the seam's render pass | the bodies |

The GL twin has exactly this shape for exactly this reason: `tagpu_posedraw_depth_unit` runs at
`tagpu_native.c:3936` and `tagpu_posedraw_unit` at `:4238`.

**No engine address, and no new one was read to build it** — everything arrives through
`tagpu_posedraw_handover`, so there is nothing for
[exe reverse engineering](../exe-reverse-engineering.html) in this landing and nothing for the hook
map above. `tagpu_vk_unit.c` is not on `thread-split.allow` and may never need to be; the list is
**unchanged** at 34 entries.

**MEASURED 2026-09-15** under system wine on the reference setup's 4070, `ss=1`, **on the binary
the two findings below produced** — every figure was taken again after them and every one
reproduced.

| | |
|---|---|
| `selbox-facings`, 4 units, **2125** ink a side, Classic++ `assets=0 shadows=0` | **0 of 786 432** |
| the same with `shadows=1` — the depth twin drawn, the map sampled | **1 of 786 432**, one channel, the same pixel every run |
| `crowd-static`, **240** posed units / 32 288 triangles, sim PAUSED, **208 699** ink a side | **64 of 786 432** |
| the same at 1920×1080, **231 426** ink | **90 of 2 073 600** |
| the same at three camera stops | 51 / 59 / 65 px — a stable **0.03 % of unit ink** |
| `shadow-struct` under **Classic** (the switch off), 5 units, **9209** ink | **20 of 786 432** |
| the caster census | 256 casters in the GL map, **240 carried** → refused, the terrain pass stands down with it, both recover |
| constraint 4 | **0 of 630 784** against `main` (`2e552d1`) with `tagpu_vk.off`, on **six** pairings, over a cross-launch floor of **0** measured on each binary twice |

#### A set may only be written in its own slot's `prepare`, so the caster draw binds a second one

A descriptor set is written during its own slot's `prepare` because that is the one instant the
seam's fence proves it is not in flight. `cast` runs *before* that, inside the shadow pass's render
pass — and at that moment this slot's main set still names the map's image from the previous frame
(the same image: the views are per slot), while that image is the render target being written. So
`cast` binds a **second, smaller set** holding the two vertex-stage buffers and **no image at
all**, and the question does not arise. Two layouts, two sets a slot, one pipeline each.

#### The caster census is arithmetic now, not a flat refusal

§2.32 refused any frame whose `otherCasters` was non-zero, which is every frame with a unit on
screen. The shadow pass now subtracts what this pass is **ready** to draw and refuses on what is
left — the replacement meshes and the native 3DO stream, both still to port.

**The two counts are comparable by construction and ours can only be smaller.** `tagpu_native.c`
counts every posed unit with `castSkip` clear; this pass counts the subset of those that reached
the hand-over *and* still have a bake mirror. So `otherCasters − casters()` is 0 exactly when every
caster in the GL map has a copy here, and positive otherwise — never negative. An over-count
refuses a frame the lane could have drawn; an under-count would draw a different map, which is the
direction that must not happen. A **short** `cast` leaves the map unready rather than partial: the
draws already recorded stay (they are in a submitted command buffer either way) but `s_liveHave` is
not set, so every consumer stands down and the picture nobody draws is the one that would have been
wrong.

#### Per-unit uniforms, which is what a unit pass is

The GL twin re-uploads one 14 336-byte pose block and a dozen loose uniforms **per unit** and draws
between the uploads. A Vulkan command buffer cannot: every draw it records is submitted together.
So each unit gets its own window in three **dynamic** uniform buffers and the draw binds the set
with three offsets.

* **The pose buffer** is `TAGPU_PD_BLOCK` (14 336) a unit **per frame slot**. That size is the
  256-piece ceiling and not the model — stock's worst is 36 pieces — but a descriptor must cover
  the block the shader declares, so the window cannot be shortened to the model. 240 units is
  3.4 MB a slot. The HAND-OVER does not pay that: its rows live in an arena and 240 stock units
  cost about half a megabyte there.
* **The small buffer** carries two vertex-stage blocks (the body's and the caster's) and one
  fragment-stage block a unit, each at a device-accepted offset. The reference device's
  `minUniformBufferOffsetAlignment` is **64**, which makes that **704 bytes a unit**; the pass logs
  the number rather than assuming it (`vk: unit: up - 4 frame slots, uniform offset alignment 64,
  704 bytes of blocks and 14336 of pose per unit, compare sampler LINEAR`).

Both are grown to the frame's own unit count rather than to `TAGPU_PD_MAXHAND` (512), and both are
given back the moment the pass will not draw — the frame that hands nothing over, and **every
refusal taken before the slot is built** (the restored atlas, a build ghost, a missing mirror, the
fog grid, the overlay, a device that will not filter the map). All of those are reached before a
byte is recorded into the command buffer, so freeing the slot there rests on the same fence the
hand-over-failed path rests on. At this size §2.28's "nothing is kept once there is nothing to
draw" is the rule that makes the pass affordable at all in a 32-bit address space, and **until the
landing review it held only for the frame that handed nothing over**.

**The leak needed a session that DREW first and then met a sticky refusal**, not one armed the
wrong way from the start: every refusal above sits above `slot_sized`, so a session launched with
the overlay on never allocated the buffers to keep. The case that fits is the restorer arming
mid-session, a build ghost appearing, or a bake eviction taking a mirror away — the pass had drawn
240 units, holds 3.4 MB a slot for them (7.3 MB at the hand-over's cap), and then stops drawing
without giving any of it back.

#### A serial, not a pointer, and not a cache slot

The GL twin draws every unit out of its **type's** two static buffers, so this pass keeps one
Vulkan buffer per baked stream and uploads it once. The table is keyed on a **monotonic serial**
`tagpu_posebake.c` now stamps on every bake, because the bake's cache slots **are** reused —
`geom_slot` evicts the least recently asked-for entry and bakes another type into it, so a table
keyed on the slot, or on the entry pointer, would hand the new model the old model's vertices. The
mirror accessor bounds the pointer against its own array, checks it lands **on** an entry rather
than inside one, and only then compares the serial; a pointer into a recycled slot answers NULL and
the unit is simply not drawn, which the caster census turns into a refusal of the whole map.

**The latch drops every entry on the 0 → 1 transition**, and that is the point rather than a cost:
an entry baked before the Vulkan lane came up has no mirror, and nothing re-bakes an entry that is
still valid, so without it the pass would stand down for ever on whichever models happened to be on
screen first.

#### Two things the measurement found, and neither was a design choice

**THE VIEW WAS READ UNINITIALISED WHENEVER NO GATHER WAS ARMED.** `tagpu_native.c` filled its
`TAGPU_FXVIEW` inside `if (fxOn || sfxOn || featOn || terrOn || markOn)` and then handed that same
struct to `tagpu_shadow_begin`, which is gated on **none** of those five. With all five disarmed —
which is exactly the configuration a single pass is *measured* in — the shadow module read stack
garbage: `zoom` came out `0.000`, its light window ran to millions of texels, and the hand-over it
publishes carried a garbage frame stamp, so the Vulkan shadow pass silently found nothing every
frame **with no line in the log**. The view is filled whatever is armed now and only the gathers
are gated. It cannot happen under the play defaults, where `terr.on` is always on.

**THE UNIT FRAGMENT SHADER READS `gl_FragCoord`, AND THIS IS THE FIRST PORTED PASS THAT WOULD HAVE
SAMPLED IT.** `TAGPU_GLSL_SCAF_TEST` (`tagpu_glsl.h`) locates the fragment in the game frame with
`gl_FragCoord.xy / uSS`. **GL measures that from the LOWER left; Vulkan measures it from the UPPER
left**, and `OriginUpperLeft` is the only execution mode Vulkan permits — so with the negative
viewport height every ported pass takes for its geometry, the two are **exact mirrors**, and the
scaffold lookup would land on the wrong end of the overlay and cut the wrong fragments through its
`discard`. Three ways out were considered and all three are worse than standing down:

* editing the macro to take the origin as a uniform changes the GL twin, which is both the oracle
  and the shipped renderer;
* re-deriving `uScafP` so the mirrored coordinate comes out at GL's value is possible on paper
  (`w = −vh`, `y = C − vpT` with `C` folding the zoom un-transform) but it is the port re-deriving
  its own inputs, and it breaks silently the day the macro changes;
* mirroring the uploaded overlay does **not** cancel — it would need the viewport's top and bottom
  margins to be equal, and they are 32 and 33.

So a frame with the overlay armed is **refused**, said once, and the reason is written down rather
than worked around. It costs nothing in play: `scaffold.on` is not in the default arm set. **Any
later pass whose fragment shader reads `gl_FragCoord` inherits this** — the hi-res path and the
effects pass both carry the same macro.

#### What the landing review found, and four of the five were the seam rather than the shader

Two Opus reviewers at `high`, one on correctness and one on synchronisation alone. **Both led with
the same finding**, independently, and it is the one below.

**A BAKE ENTRY THIS FRAME STILL NEEDS COULD BE EVICTED TO MAKE ROOM FOR ITS OWN SIBLING.**
`vb_slot` refuses to evict an entry stamped with the frame in hand, and `upload` stamped the pair
*after* resolving both — so a unit whose geometry was cached and whose material was not handed
`vb_slot` its own geometry entry as the least recently drawn. The entry is retired and reused for
the material, `g` and `m` then name the **same** entry, and the body draw fetches 32-byte vertices
out of a buffer holding 20-byte ones: past its end, on a unit that still passes the
every-unit-or-none gate because it *was* drawn. The stamps moved to the instant each entry
resolves. The same eviction had a second mouth: `stageNeed` is counted before a byte is uploaded,
so an eviction inside the loop turns a later unit's cache hit into a miss whose bytes were never
reserved, and the staging `memcpy` runs past the mapped allocation. That one is now **bounded
against `s->vscap`** rather than against the pre-pass, because a pre-pass cannot budget for the
eviction it is trying to budget for; a unit that will not fit is not drawn, and the gate below
turns that into a refused frame — and the bound carries the **atlas's reserved share** of that same
allocation with it, because `atlas_upload` copies into the tail of it after the loop and an
overspend small enough to leave every unit fitting would have run the atlas memcpy past the end
instead. Reachable once the 512-entry table fills, which two map loads do.

**THE COMPARE-SAMPLER REFUSAL WAS TAKEN ONE HOOK TOO LATE.** It lived in `prepare` — which runs
*after* `cast` has put this frame's casters into the map and after the shadow pass has published
it. So on a device that will not filter a depth format linearly the bodies stood down while the
terrain pass went on sampling a map those bodies are in: unit shadows lying on terrain with no
units above them, every Classic++ shadow frame, silently. It is decided in `upload` now, where
`tagpu_vk_unit_casters` still answers 0 and the census refuses the map with it. **The hook that
owns a decision is the earliest one that can take it**, not the one that needs the answer.

**THE WORLD SCISSOR WAS NOT SCALED.** The rect arrives in game-frame pixels and the viewport covers
the whole attachment, exactly as in §2.29–§2.31 — and `unit_scissor` was `feat_scissor` without its
`w/gw`, `h/gh`. At the 1:1 sizes an A/B is run at the two are the same number, which is why 786 432
pixels of measurement could not see it; a 640×480 game frame in a 1920×1080 window would have
clipped every unit to the left third of the bottom quarter.

**A BARRIER SAT BELOW A GATE THAT CAN RETURN.** The `TRANSFER_WRITE → VERTEX_ATTRIBUTE_READ`
barrier was recorded after the every-unit-or-none gate. That gate returns without drawing, but the
copies are already in the command buffer and the buffers keep their serials — so a later frame
finds them, uploads nothing, and reads vertices no barrier ever ordered against the write that
filled them. Recorded before the gate now.

The fifth is the give-back above. **Nothing was rejected**; the two minor notes (a latch shared by
two different messages, and a comment claiming the fog cell count is "not always `cols*rows`" when
every assignment makes it exactly that) were both corrected.

#### The scaffold question: half answered, and the half that is says which half is not

The handoff asked this landing to decide, once, how a pass samples another pass's texture — for the
units, the hi-res path and the effects. **The mechanism is settled and is in place**:
`tagpu_vk_scaffold.c` exposes a **frame-stamped per-slot `VkImageView`** in exactly the shape
`tagpu_vk_shadow.h` arrived at, its `prepare` moves to the top of the seam's frame while its
`record` stays over the world, and this pass points binding 44 at the real image. **Prepare order
is not record order**, which is new to the seam and stated at both call sites.

What is *not* settled is the paragraph above. Binding 44 names the overlay anyway, so it is the
descriptor that will still be right when the fragment-coordinate half is answered.

#### The residual, and it is measured rather than asserted

The 64 px are a **stated bar**, of the same family as the effects pass's line tie-break (§2.31) and
the terrain pass's one-pixel lambert (§2.30, §2.32) — but larger, because a unit frame is 32 288
triangles of dense sprite art rather than a terrain grid. What was established about it:

* **Deterministic.** On a frozen sim each lane is **byte-identical to itself** across two captures,
  and the differing **set** is the same both times. So it is not a race, not an ordering, and not a
  mirror that had not converged.
* **Per pixel, not per face.** **48 of 55** 8-connected clusters are **one pixel** and the largest
  is **three**; there are **58 distinct (GL, Vulkan) colour pairs across 64 pixels**. A flipped SHD row would
  repaint a whole triangle uniformly and give a handful of pairs, so the shade quantisation is not
  what this is.
* **Mostly colour, a little coverage.** **5 of 64** have one lane black; the other 59 are colour
  changes at pixels both lanes covered.
* **Not the lighting and not the map.** It is there with the lambert on and off, with the shadow
  map on and off, and under **Classic** with the whole Classic++ switch removed.
* **A texel boundary, looked at.** Zooming one of the runs shows a **horizontal texture-row
  boundary picked one row apart** along eight pixels — the GL lane takes the dark row under a light
  band, the Vulkan lane continues the light one.

That last one is **§3.0's own finding in a second lane**: a fragment centre landing exactly on a
texel boundary, which the terrain pass fixed with a 1-texel replicated border **plus**
`TAGPU_EDGE_NUDGE`, a 1/32 game-pixel offset in its vertex shader. **The unit path has no nudge**,
and the 16.16 snap puts posed vertices on an exact grid over art whose UVs are exact texel
multiples, so a great many fragment centres land exactly on a boundary. In one lane the tie goes
one way and in the other it goes the other.

**The candidate fix is therefore the terrain's, and it is not this landing's to take.** Adding the
nudge to the unit vertex shader would change the **shipped GL renderer** to serve the port, which
is the owner's decision and not a session's; the terrain's own note records that its 1× output came
out bit-identical, which is evidence the cost may be nil, not proof of it for units.

**What is NOT established** is that the last-bit difference is the two compilers rather than
something in the port. The lambert, the shadow map, the restored atlas, draw ordering and every
per-face term are eliminated; the interpolated value itself cannot be read from here, and this lane
still has no validation layer.

#### Not covered

* **The nanoframe WIRE, the Classic SILHOUETTE and the structure SLANT** — the same program and the
  same bake at `uRange` 2 and 1. *(All three are drawn now: the silhouette and slant since
  2026-09-22, the wire since 2026-09-23 — [build-state](../build-state.html) §7. What follows is the
  state this section measured.)* They draw *outside* the window the A/B brackets, so they cannot
  make this comparison disagree, and the pass does not refuse for them; they are simply missing
  from a Vulkan frame that has them in the GL one. The wire also draws **lines**, which is §2.31's
  stated bar.
* **The replacement meshes** (`tagpu_hires_draw.c`) **and the native 3DO stream's own unit
  vertices** — the effects models and the selection lines. Both still count into the shadow map's
  census, which is what refuses a frame with either in it: measured at 256 casters against 240
  carried on `crowd-static`, whose 16 ARMPWs take the hi-res path.
* **The BUILD GHOST**, which rides the same entry points in a second window: the hand-over counts
  it in `otherDraws` and the pass stands down.
* **Classic++ `assets=1`.** The refusal is written and is the same shape as the feature and terrain
  passes', but it was **not exercised**: the restorer never armed the unit twin in this session, so
  `uRestored` stayed 0 on both lanes and the frame was drawn rather than refused.
* **`ss` 2**, as for every world pass (the two captures would be different sizes).
* **The owed-teardown path**, which has still never fired on any pass, and the per-type buffer
  retire, which needs an eviction the 512-entry table did not reach.
* The validation layer, still, and no device but the 4070.

### 2.34 The UI layer's 1x mirror, drawn by Vulkan (`tagpu_vk_gui.c`, `tagpu_gui_surf.c`'s op mirror) — Phase G, G19f landings 1–5

**The first pass of G19f, and the first ported thing that is not a draw over a mesh.** §2.3e's UI
layer is a **stateful store of per-surface twins** that an op stream mutates, plus a three-layer
composite. The state is the whole difficulty and it is what decided the cut:
[the G19f plan](../g19f-plan.html) has the landings.

**Nine shaders, none of them new.** G19c already translated `QVS`, `CPY_FS`, `SPR_FS`, `STR_FS`,
`CURS_FS`, `MM_FS`, `SHARP_FS`, `LAY_VS` and `LAY_FS`. Landings 1 and 2 use **five** of them —
`QVS`, `CPY_FS`, `SPR_FS`, `STR_FS` and the `LAY_VS`/`LAY_FS` composite — and write no GLSL.
`CURS_FS`, `MM_FS` and `SHARP_FS` are the sharp layer, which is landing 3.

**Landing 1 was the mirror with `PK_STRING` refused; landing 2 is the string op.** They are one
section because they are one pass and the second is the first's machinery with one more program.
The distinction that matters to a reader is that **landing 1 alone composited nothing in real
play** — text is on screen in essentially every in-game frame, so the capability refusal below
fired on all of them and only `gui.on=nostring` produced a picture to compare. Landing 2 is what
made the pass a thing that draws the game's UI rather than a thing that draws a fixture's.

#### The op stream is dead by the time the Vulkan lane runs

`render_ogl.c`'s iteration is `tagpu_packet_acquire` → `tagpu_overlay_draw` (inside which
`tagpu_gui_present` drains) → `tagpu_packet_frame_end` → `tagpu_vk_frame`. `drain()` advances the
queue's arena tail **per op**, so the game thread may overwrite the bytes those ops point into the
instant it returns — two calls before the Vulkan lane exists. A pass reading `g_guiq.arena + aoff`
would be §2.30's fog-grid bug on a 16 MB buffer.

So `tagpu_gui_surf.c` grows an **opt-in mirror**, filled inside `drain()` as each op is applied,
carrying a copy of the op and of its payload. The alternative — deferring the tail publication
until after `tagpu_vk_frame` — was rejected because the release would have to run on every path
through `tagpu_overlay_draw` including its three early returns, and it would make the GL queue's
lifetime depend on a lane that is normally not armed. **Nothing is copied until
`tagpu_gui_mirror_want(1)`**, which is the only reason copying an op stream that can run to
thousands of ops a present is affordable at all.

**It is recorded where each op is APPLIED, not where it is read.** The hand-over says what the
render half DID: a sprite whose atlas entry would not resolve drew nothing, and a copy whose
source has no twin is a reseed rather than a draw. And it is a **public struct of its own**
(`TAGPU_GUIOP`), not the producer's private queue op, so a change to the queue cannot silently
change the port's contract.

Three things ride along that the port must not re-derive: **the atlas rect the GL lane resolved**
for each sprite (a second lookup could answer differently after a repack, and the A/B would then
be comparing two atlases), the UI atlas's texels through `tagpu_gaf.c`'s existing CPU mirror, and
**the engine's own frame** — the composite's bottom layer and its stale-mirror guard, which the GL
lane has as `f->surface_tex`, a GL texture, and which is therefore copied out of the fork's primary
under `g_ddraw.cs` on the same lifetime argument §2.3f makes for the palette.

#### The flip is a property of presentation, and this is its clearest case

§2.32's rule decides every coordinate question here. The **twin draws take no flip**: their target
is SAMPLED, and `QVS`'s `y / uSize.y * 2 - 1` puts quad y = 0 at attachment row 0 under both APIs.
`CPY_FS` reads `gl_FragCoord` — the macro §2.33 had to stand the unit pass down for — and with no
flip GL measures it from NDC −1 while Vulkan measures it from attachment row 0, **which is the
same row index**. So it ports unchanged and §2.33's refusal does not arise. The **composite does
flip**, because the composite is presented.

That was derived against `twin_copy` and `QVS` rather than reasoned from conventions, and the
first draft of the plan got it wrong in the other direction — it claimed the GL lane already
contained a compensating mirror. It does not. There is no flip anywhere in `tagpu_gui_surf.c` and
the module's own comment says so.

#### The twin store is the first shared mutable state on this lane

Every world pass through G19e keeps its state **per slot**, so the seam's `fence[slot]` wait is the
whole argument. A twin cannot be per slot: it is persistent state an op stream mutates across
frames, and 32 of them per slot is the memory this phase spends its budget measuring. So they are
shared — and frame N writes them while frame N−1 may still be sampling them in its composite, a
write-after-read across submissions.

**The DATA hazard is closed by an ordering.** A `vkCmdPipelineBarrier`'s first synchronisation
scope includes every command submitted previously to the same queue, so one barrier at the top of
the replay — `FRAGMENT_SHADER`/`SHADER_READ` before
`COLOR_ATTACHMENT_OUTPUT`/`COLOR_ATTACHMENT_WRITE` — orders this frame's twin writes after every
earlier frame's composite reads. Both landing reviewers checked that argument and neither could
break it. **There is still no validation layer in the wine prefixes**, so it is read against the
spec and not checked by one.

**AND IT SAYS NOTHING ABOUT LIFETIME, WHICH IS THE MISTAKE THIS SECTION ORIGINALLY MADE.** Ordering
a write after a read does not keep the object alive to be read. The first version of this pass
destroyed a twin's image, view, memory and framebuffer inside `prepare` — while `slots − 1` earlier
submissions were still executing and while the command buffer being recorded already named them —
and did the same to the shared atlas and engine images on a dimension change. A `PK_FREE` for the
surface the previous frame presented is a use-after-free on the **first** eviction. **Both
reviewers led with it, independently**, and the file already contained the correct rule: the
`refuse` label destroys nothing and says why.

The answer is the lane's own **slot-bitmask retire**, the one §2.30's terrain pass and §2.33's unit
pass already use — every slot's bit set at push including the one being recorded, cleared when that
slot comes round again behind its fence, destroyed at zero, and a full list stops the pass rather
than destroying anything. **No Vulkan OBJECT is destroyed from `prepare` any more** — the two
`kill_buffer`/`slot_free` sites that remain are the slot's own host-visible buffers, which are
safe under the fence the seam already waited on and for which every world pass makes the same
argument. That sentence read "no destruction at all" until the re-review pointed out it was false
and, worse, told the next reader not to look — which is exactly how the original bug got in.

#### The replay runs on frames the composite cannot

**This is the landing's own hardest-won line, and the first version of it was half a fix.** The
refusals — a Classic++ colour twin, coverage in the sharp layer, a missing engine frame — first
returned before the replay. But the GL lane applies those ops whatever it draws, so a frame the
port skips leaves its twins behind the GL lane's **for the rest of the session**, silently. Those
three gate the composite now and the replay runs regardless.

**What the review found is that five other paths had the same shape and were missed**, and that a
sixth was on the producer's side:

* `standdown` dropped the store and asked for nothing, so the next frame found no presented twin,
  landed there again, and **the pass never drew again for the session with no line in the log**.
* Pass 1's eleven bare `return 0`s did it for one malformed op.
* An abandoned mirror frame (the op array or arena refusing to grow) did it silently, counted by a
  statistic nobody read.
* **`PK_STRING` is the one that matters most**, because it is not a bug in a rare path: the GL lane
  *applies* it (`twin_string`) and landing 1's hand-over did not carry it, by design. Treating it as
  a `compose = 0` meant those glyphs were in the GL twin and would never be in ours. **The sentence
  that stood here — "the twins are kept level" — was written of the ops the mirror carries and was
  false of the one it does not.** Landing 2 carries it, so this is now a statement about the shape
  of the refusal rather than about strings: `otherOps` is **0 for every op kind that exists today**
  and the machinery stays, so the next kind added to the queue lands there rather than being drawn
  wrong. A capability gap asks for **nothing** — a fresh start cannot help when the very next frame
  carries another one — and the store is caught up with one fresh start when they stop.
* And `mir_finish` published nothing when `draw_layer` returned early, on frames whose ops the drain
  had already applied — the same hole the consumer had just been restructured to close, still open
  at the other end. It publishes with `presented = 0` now: replay, do not composite.

So **falling behind is a STATE**, with one way in and one way out: drop the store, raise the
producer's own `reseed`, composite nothing, and apply nothing but the RESET that answers it. That
is `tagpu_gui_surf.c`'s own `s_skipToReset`, for the same reason. **Measured firing and recovering**
on the binary the fixes produced.

**"ONCE, ON THE TRANSITION" IS ONCE PER FRAME WHEN THE TRANSITION REPEATS, AND THAT IS THE RESEED
STORM AGAIN.** The re-review of landing 1 caught `behind()` raising the producer's flag every frame
and made it act on the transition only. Landing 2's review found the same storm through the other
door: a RESET **clears** `s_behind`, and a condition that is structural rather than transient fires
again inside the very frame that answered it — so the pass asks, is answered, asks again, at the
frame rate, and the thing being reseeded is **the GL lane's own twin store**. The asks are capped
now, and the cap counts **fruitless** asks: a run of frames that got through **the replay** is the
evidence the last fresh start worked and gives the budget back, so a map change still recovers while
a structural condition goes quiet after eight tries and never touches the producer again. A
capability gap costs the oracle nothing; that is the point of telling it apart from a sync gap.

#### Two colour disagreements are neither

They are not the behind state either. Classic++ colour is decided
on the producer from `tagpu_gui_col_ready`, the consumer's answer of the **frame before**, so the two
disagree for one frame whenever that answer moves — in both directions, not only "off to on" as the
code once said. They are answered where they arise, never with a fresh start:

* **A restored sprite over a blanked atlas.** `restore_want` runs *between* the ops' validation and
  their replay in `tagpu_vk_gui_prepare`, and every job that is not a repaint blanks the restored
  atlas the frame it starts — every atlas re-lay. The replay draws such a sprite with
  `uRestored = 0`: `SPR_FS` writes alpha 0 to the colour plane, which `LAY_FS` resolves through the
  palette. Dithered art, never wrong art; the consumer's answer drops to 0 that frame, the
  producer's validity follows, and its 0 → 1 edge asks the engine for the repaint that brings the
  colour back (`col_ask_repaint`). Logged once per stretch: `gui: a restored sprite arrived while
  this lane holds no restored atlas - drawn indexed until the repaint`. That frame's composite needs
  no atlas: `uColOn` reads the twins' colour planes, which the blanking did not touch. A frame whose
  producer says colour while the lane holds no atlas at all would be composited **indexed**
  (`uColOn = 0`) rather than skipped, but that branch is a brace: every `tagpu_gui_col_ready(1)` is
  `s_arHave` at the end of the previous prepare, so it needs a teardown between present and prepare.
  Every path in `restore_want` that gives up, or leaves an image laid out for another atlas, clears
  `s_arHave` before it answers.
* **`colourTwins` for a twin that has no colour plane.** The header defines it as colour valid
  **and** the presented twin carrying one; the producer published validity alone, so every surface
  presented after a reseed — fresh on both sides, colour on neither — was a stand-down. It is
  `s_colValid && t->col` now. `t->col` and the consumer's `colImg` are made by the same
  `TAGPU_GUICOL_DST` op and dropped by the same RESET, so they agree **as long as every record reaches
  the consumer**. The picture store's palette rebuild wants validity alone, and reads it as `colValid`.

**Open: a record the consumer never takes.** `mir_begin` drops an untaken record rather than
publishing it `lost`; one goes untaken when the swapchain acquire returns out-of-date before
`tagpu_vk_gui_prepare` runs (a window drag, a same-size rebuild). Its ops were applied to the
producer's table and never to ours: a `DST` there makes the colour disagreement above (one stand-down
and one fresh start), and ops that only draw into existing twins leave stale art until the engine
redraws it. Publishing `lost` would ask a fresh start per dropped record, which a drag can make many
of; the by-construction answer is to carry the untaken ops into the next record, and it is not built.

Answered with a fresh start, each disagreement re-laid the atlas, which started the next job, which
made the next disagreement: **eight asks, then the capability-gap mute, then nothing composited for
the rest of the session**. MEASURED 2026-09-27: Total Mayhem's skirmish screen on Windows went black
that way (v0.3 and after); on the Wine suite every Escalation run reached the mute and Total Mayhem
and ProTA asked 3 to 6 times — invisible to a suite that reads state and gadgets, not pixels. After:
the only asks left are the start-up's two (`an op names a surface this store never seeded`).

**The check is `gui-stress`** (`tools/compat/setups.json`): Total Mayhem with `reseedstress=120` in
`tagpu_gui.on`, a harness lever that raises the producer's `reseed` every 120 presents through the
consumer's own call. Colour is valid before each one (the restore of three frames lands in well
under the interval), so each one meets both races. It passes only with **no** ask of the consumer's
own after the lever's first fresh start, no mute, the lever seen to fire at least ten times, and the
indexed fallback seen at least once — a stress run that never met the race proves nothing.
MEASURED 2026-09-27 on Wine: the build without the fix asked 7 times (`a restored sprite and no
restored atlas on this lane`) and gave up; the fix met 22 fresh starts with 22 indexed fallbacks and
no ask. Every other setup's run fails as well if its UI layer gave up or asked past the start-up
(`gui_health` in `tacompat.py`).

**"A run of frames that COMPOSITED" is what that said first, and the re-review disproved it out of
this very page.** `compose` is 0 whenever the sharp layer has coverage — *"every frame with a cursor
on screen"*, as "Not covered" below already stated — so the refund sat on a branch an ordinary
session never reaches, every legitimate transition counted against the cap, and four level loads
muted the pass for good. Being **level** is a property of the replay, not of the composite. The
evidence against the claim was two screens down in the same section.

**AND A LOST FRAME NOW RE-ASKS.** `g_guiq.reseed` is **one-shot**: the producer clears it the
instant it publishes the RESET. If the present carrying that RESET is itself abandoned — and it is
the likeliest one to be, being every surface seeded at once and so the largest arena there is — the
answer never arrives, `s_behind` is already 1 so nothing asks again, and the replay skips every op
for the rest of the session waiting for a RESET that will never be sent. Publishing the lost frame
(above) turned *silently* behind into *permanently* behind with one line in the log. A lost frame
therefore invalidates an outstanding request. That is not "the window is small": it is a message
provably not delivered, re-sent.

**A REFUSED FRAME THAT CARRIES THE RESET RE-ASKS TOO, and pass 1 now starts where pass 2 does**
(2026-09-23). Pass 2 skips every op before the RESET while behind; pass 1 validated all of them
anyway, so one op it refused discarded the frame that carried the fresh start, and `behind` stays
silent while already behind. Seen as the post-game DEFEAT screen (`ENDMSN.GUI`) staying black for
good: the render thread restarts for the 640x480 shell, the rebuilt pass asks for a fresh start,
and every frame of that screen carries a **whole-surface identity self-copy** (surface onto itself
at the same origin), which pass 1 refuses because a self-copy with a *different* origin reads
and writes one image in one draw. Two changes:

- `tagpu_gui_hook.c`'s `publish` does not emit an identity self-copy. It writes every pixel with
  its own value, so not sending it leaves every twin byte exactly as the copy would. **This is the
  one that fixes this screen**: the self-copy also follows the RESET, so the change below alone
  would re-ask on every fresh start until `BEHIND_ASKS_MAX` muted the pass.
- `tagpu_vk_gui.c`'s pass 1 starts at the frame's first RESET while behind, and every refusal in
  it goes through `behind_ex(…, carries)`. A refusal on a frame that carries the RESET is the
  answer provably not applied, the same as `h.lost`, so it asks again. This closes the general
  hole: an op refused ahead of the RESET no longer strands the pass, and one refused after it is
  retried and ends in the mute's log line rather than in silence.

Measured on `scenarios/mex-ghost.json` at 1920x1080 by self-destructing the Commander: before,
the census read `gui=0` on every frame of `ENDMSN.GUI`; after, `gui=1`, and the stats rows, the
buttons and the cursor composite.

**The backdrop, the title and the column labels were still black, and the reason is one sprite.**
`ENDMSN.GUI` loads `bitmaps\outcome0.PCX` into its own 640x480 surface (the backdrop, the column
labels, the frame) and draws exactly **one** op onto it: a GAF, the DEFEAT/VICTORY title, 101x29 at
(272,15). The census reads 304 051 of 306 976 changed pixels unexplained on that surface (the PCX)
and one op. That op revoked the asset claim before the bytes had crossed, so the surface seeded
empty; and the engine's repaint copies this surface onto the frame every time, from a source that
after the restart's reset was neither seeded nor an asset. **Fixed by `snap_take`**
(`tagpu_gui_hook.c`): when the first op revokes a claim whose bytes never crossed, `op_add`, which
every leaf calls from its `before_` observer, copies the surface BEFORE the engine writes, so the
copy is the loader's bytes and not composed pixels. `pub_seed` then sends it as a `PK_ASSET`
followed by every sprite drawn on it since (each with its own plane, in `ovl`), and a copy whose
source is such a surface seeds it on demand. Any draw it cannot replay drops the snapshot, which
is exactly the old behaviour. MEASURED on the same fixture: backdrop, title, labels, frame, the
stats and the Main Menu button all on screen, the main menu after it correct, the in-game UI
unchanged. Design in [GUI renderer](../gui-renderer.html), under `PK_ASSET`. **A colour repaint
re-seeds it too** (2026-09-26): in Classic++ the seed's sprites land in the same window, before
the picture store has restored the backdrop, so `repaint_arm` raises `reseed` on every surface
holding a snapshot at each `colarm` edge, and the re-sent seed finds its picture done
([GUI renderer](../gui-renderer.html) §27.2). The flag is apart from `seeded`, which is what owes a
freed surface's twins their `PK_FREE`.

**The player names on the coloured bars (closed 2026-09-23).** They are drawn through `0x4B8310`,
which was recorded as `OP_GAFB` and published as box bytes the drain drops, so they reached no screen
on the Vulkan lane. The first reading of that blit (the paragraph that stood here) took the raw
arm's `dst = table[src·256 + dst]` for the whole function and called it a blend that needed a new
destination-reading op. A probe of the draws said otherwise: all 18 glyphs are **RLE** frames, and
the RLE arm `0x4CC3D0` writes `row[src]` over each texel it draws with `row = table + 15·256`,
reading nothing under it. So what reaches the screen is a fixed picture, the glyph remapped through
one row, and `gafb_capture` (`tagpu_gui_hook.c`) builds exactly that picture on the game thread,
inside the engine's call:

- it decodes with **coverage** (`tagpu_gaf_decode_cov`), because a literal texel equal to the key is
  drawn by the engine and a plain decode cannot tell it from a skip;
- it picks a key that **the row cannot produce from any byte** (the frame's own when it is free),
  so no drawn texel can read as transparent, and a later sight that skips the decode still knows it;
- it folds the row's **bytes** into the identity, since the engine rewrites the table in place.

From there the op is a keyed sprite, and the sprite branch of `publish`, the seen table, `ovl` and
the consumer take it unchanged. The observer now also records nothing when bit 7 is clear (the
engine draws nothing then). MEASURED on the same fixture: all **788** glyph texels identical to
`tacli shot`, and in-game `gafb` is 35 px of area over a whole match, so carrying it costs nothing.
**Still not carried:** the raw arm (it reads its destination, so it needs the tint's snapshot
machinery with a per-texel row) and sub-frame stacks through `0x4B8500`. Neither was reached on
any screen measured. The 58 sprites the shell reports lost right after launch predate this change:
the previous commit's build loses the same ones.

**Three `continue`s were doing the same thing quietly, since landing 1.** An op naming a surface
this store never seeded, and a pixel op outside its own twin, were skipped — the GL lane applies
them and we never do. The composite's own `tw_find(h.presented)` catches that for the **presented**
surface only, and only on a frame it draws. They are the behind state now, and the fixture shows
they were firing the whole time: the two events a shell→game transition produces used to report
*"the presented surface has no twin here"* and now name their actual cause.

#### The string op is the twin store's, not the sharp layer's (landing 2)

**`twin_string` stamps TA's own glyphs into the TWIN** (G17d), from a per-font glyph cache the
render thread rasterises. §2.3e's "the sharp layer is empty until the cursor and the string op fill
it" is about the *sharp* text path; this one is a draw into the same `RG8` twin every other op
writes. So the port is landing 1's store plus one pipeline — `STR_FS` on the same descriptor layout
as `SPR_FS` and `CPY_FS` — and not the device-resolution layer the plan had bundled it with.

**The texels needed no mechanism.** `tagpu_text.c` already keeps its glyph atlas as a CPU array
(`s_gatlas`) and uploads the GL texture *from* it. `tagpu_text_glyph_atlas()` hands back that array
and its dimensions — one accessor where the feature pass needed a whole opt-in mirror for the same
question.

**BUT IT NEEDED A SERIAL, AND THE ONE THAT WAS ALREADY THERE WAS THE WRONG ONE.** The first draft
keyed the `R8_UNORM` upload on `tagpu_text_glyph_gen()`. **That counts REPACKS** — the two paths
that throw the whole atlas away — and says nothing about an ordinary glyph being rasterised into a
fresh shelf, which changes the atlas's bytes and leaves every previously issued cell valid. The GL
lane never had to tell the two apart because it re-uploads on `s_gdirty`, a plain dirty bit. So the
Vulkan image was uploaded **once** and every `(font, code)` pair first seen afterwards — a new digit
in a resource counter, a unit name not yet displayed, a second font — stayed **0** in it: the glyph
invisible where `bg == tr` and a solid `bg` box where it is not, permanently, for the session, with
no counter anywhere saying so. `tagpu_text.c` now publishes a **content serial** beside the
generation, bumped wherever `s_gatlas`'s bytes change and nowhere else (not when the GL *texture* is
re-created, which is liveness). **Both landing-2 reviewers led with this, independently** — the
sixth such pair on this lane, and the measurement could not have found it: the fixture's whole glyph
set was in the atlas before the first upload, which is exactly the shape that reads 0 px.

**AND A REPACK MID-PRESENT INVALIDATES EVERY STRING ALREADY RECORDED IN IT.** `twin_string`'s retry
is correct *for itself*: it drew into the GL twin from the texture as it stood at the generation it
resolved against. The mirror does not draw until the drain is over, against the atlas as it stands
**then** — so a repack caused by a *later* string in the same present leaves the earlier strings'
cells naming cleared texels. The producer records the generation with the first string of a frame
and **loses the frame** if it has moved by `mir_finish`. Which introduces the third fix:

**AN ABANDONED MIRROR FRAME USED TO BE WITHHELD, AND A WITHHELD RECORD IS INDISTINGUISHABLE FROM AN
UNARMED LANE.** The consumer's "nothing handed over" branch reads it as *the GL lane drew nothing
either* — true when the lane is not armed, false when the op array or the arena refused to grow
mid-frame, and in that case the store is a frame of ops behind, silently, for the session. The
record is published now with a `lost` flag and the consumer answers it with the behind state. This
is the same hole the landing-1 review closed at the `!s_mLayer` end of `mir_finish` and left open at
this one; it was found verifying the repack fix, and **neither reviewer named it**.

**THE CELLS ARE CARRIED, NOT LOOKED UP, and for a harder reason than the sprite's.** The sprite's
atlas rect is carried because a repack between the two lanes' lookups would answer differently.
A string is worse: `twin_string` resolves its glyphs against an atlas that can **repack in the
middle of one string** — it takes the generation before and after and retries once for exactly
that — so a second lookup here could name texels that moved while the GL lane was drawing, and the
A/B would be comparing two atlases rather than two rasterisers. The hand-over carries the four
`short`s a glyph that the GL lane settled on, `nglyph` of them in the mirror arena, plus:

* **the pen it started from**, which is `o->sl` and `o->st − yoff`. `0x4CCF60` writes its first
  pixel row at `y − (s8)font[2]`, not at the `y` it was given (§"The in-game bitmap font" in the
  engine map), so the *resolved* top is carried rather than the op's.
* **the three colour bytes** `fg`, `bg` and `tr` — the blitter's own three arguments, which
  `STR_FS` takes as `uFg`, `uBg`, `uTr`.

`mir_string` is called from the **tail** of `twin_string`, after the draw succeeded. A string that
stamped nothing takes the `reseed:` label instead and raises the producer's own `g_guiq.reseed`, so
both lanes are level: neither drew, and the RESET that answers it reaches the mirror as an op like
any other. An arena that will not grow abandons the mirror frame, which is `behind()`'s business.

**A STRING SETS NO SCISSOR**, because `twin_string` calls `glDisable(GL_SCISSOR_TEST)` outright
where `twin_sprite` and `twin_copy` enable it. The port sets a full-twin scissor rather than the
op's box; clipping to the box would cut glyphs the GL twin has.

**One block, `nglyph` quads.** Every glyph of a string shares its three colours and the twin's
size, so the descriptor set is bound once and the vertex offset walks — which is what the GL lane
does with one program and n draws. That made the vertex count stop being the draw count, so the
**vertex slots and the uniform windows are counted separately**: `nquad` sizes the vertex buffer
(and positions the composite's own quad at the end of it), `ndraw` sizes the uniform and flags
buffers. They were one number through landing 1 because every draw was exactly one quad; keeping
them one here would have put the composite's own quad, which sits at the end of the vertex buffer,
on top of a glyph's.

#### The sharp layer, and why it needed none of the twins' machinery (landing 3)

The third client of the composite is a single `RGBA8` at **device** resolution, row 0 the
viewport's top, that the GL lane clears to `(0,0,0,0)` at every present and the composite takes
wherever its **alpha** says it has coverage. Landing 1 wired `uSharpOn` and left it 0; landing 3
turns it on.

**IT IS PER SLOT, AND THAT IS ITS WHOLE SYNCHRONISATION ARGUMENT.** A twin had to be shared
because it *accumulates* — which is what forced the barrier reasoning above, the slot-bitmask
retire, and the use-after-free that got through the first time. This layer keeps nothing across
frames, so it is per-slot state that the seam's own `fence[slot]` wait already covers, and none of
that reasoning applies to it. Its render pass is the twins' opposite — `CLEAR` where they `LOAD`,
for the opposite reason — and the clear is *transparent* rather than merely black, because alpha is
what the composite gates on. It takes **no flip**, because like a twin it is sampled rather than
presented (§2.32).

**What crosses is a short ORDERED LIST of quads, not texels and not an op stream**, because that is
what the layer is: three clients drawn in a fixed order — the harness's flat quads, the cursor, then
the minimap's base and its view box. At most 16, carried **by value**, which retires any question
of what they point into.

**THE CURSOR'S RECT IS WHY THE LIST CARRIES RESOLVED GEOMETRY.** `sharp_cursor` takes the pointer
position from `mouse_last_client()` **at draw time**, and the Vulkan lane runs later in the same
iteration of `render_ogl.c`'s loop — so re-deriving it there places the cursor where the mouse has
moved to *since*. That is a guaranteed non-zero A/B which would have read as a rasteriser
difference. The resolved destination rect crosses instead, with the atlas rect and the colour key.
**Same rule as the sprite's atlas rect and the string's glyph cells, and the third landing on this
pass it has decided.** The minimap's box (the packet's, after the HUD map and the `hq8` scale), its
view rect (`main+0x142CB`, four edges at one *game* pixel each so the box keeps its weight as `k`
grows) and that box's colour, already through the presented palette, cross for the same reason.

**Two things reading the code corrected, each of which would have cost a measurement round:**

* **The engine's minimap pair is `RGB8`, not `RG8`.** The GL source calls it "the engine's two
  bases" and the plan repeated that, but `MM_FS` reads **three** channels — `r` the fogged base,
  `g` the unfogged one, `b` the composite the engine drew its dots, radar arcs and points into —
  and the CPU loop beside it strides by 3. Both minimap images are widened to `RGBA8` on the way
  into Vulkan, because `VK_FORMAT_R8G8B8_UNORM` is optional and rarely supported while the
  four-channel one is universal, and `MM_FS` reads `.rgb` either way.
* **`SHARP_FS` is needed by this landing**, which the plan doubted on the grounds that `LAY_FS`
  already samples the layer. It is not the sampler; it is the *flat quad*, and the minimap's view
  box is four of them.

**One descriptor layout serves all three programs**, because G19c's translation had already
normalised them: a 16-byte block at binding 32 each, and samplers at 40..42 for `CURS_FS`
(`uAtlas`, `uAtlasRGB`, `uPal`) and `MM_FS` (`uPic`, `uEng`, `uPal`). So the sets are indexed by
**kind** rather than by draw — a frame's up-to-16 quads sample at most three distinct combinations
of images — which is three sets a slot instead of sixteen.

**What the landing review found, and the one that could not be measured.** Eight findings across
two reviewers, and **both led with the sampler independently** — the seventh such pair on this lane:

* **THE MINIMAP PICTURE IS MINIFIED `LINEAR` IN GL AND WAS POINT-SAMPLED HERE.** This module had one
  sampler and a comment saying every texture it reads is nearest. That was true until this landing:
  `MM_FS`'s own comment explains why the picture is the exception — the destination box is smaller
  than the 252-px picture at every `k` it draws at, so every fragment takes the *minification*
  filter, and a downsample wants one. GL blends four texels there; a nearest sampler takes one. A
  second sampler is the whole fix, and the A/B could not see the bug for the reason above.
  **THE FIRST VERSION OF THAT FIX WAS A NO-OP AND THIS SECTION RECORDED IT AS CLOSED.** The new
  sampler inherited `maxLod = 0.0f` from the nearest one it was copied from; Vulkan clamps the
  level-of-detail to `[minLod, maxLod]` and only *then* decides magnification (λ ≤ 0) against
  minification, so with both 0 the answer is always magnification and `magFilter` — still nearest —
  is always what runs. `VK_FILTER_LINEAR` never executed. **An open divergence written down as
  closed is worse than one written down as open**, and `tagpu_vk_unit.c` already carried
  `maxLod = 0.25f` for exactly this reason, in this repository. [The re-review caught it.]
* **THE ENGINE'S MINIMAP PAIR WAS ALIASED PACKET MEMORY, NOT COPIED.** The comment claimed the
  hand-over's frame rule covered it. It does not: the packet's rule is **stricter**, and
  `tagpu_packet_frame_end` gives it back *before* `tagpu_vk_frame` runs in the same iteration — so
  the consumer read it after its owner released it. `tagpu_packet.poison` fires at that give-back,
  which means the lever built to catch exactly this kind of mistake could not see it either. It is
  copied now, as `eng`, `mmPic`, the atlas and the palette all already were. Its **dimensions come
  from the same read as its bytes**, so the two cannot disagree.
* **A refusal that still consumed its input.** The minimap guard cleared `compose` but left
  `needMM` set, so the two cases it names — a null pointer, a dimension past the cap — were fed
  straight into the staging path below, which reads `w * h * 3` bytes from the very pointer it
  refused.
* **`h.nsdraw` sized two allocations before its own validation gated anything**, which is this
  file's own stated rule ("a handed-over count never sizes an allocation") being broken.
* **`s_palView` was the one view bound with no `Have` guard**, so a null handle could reach
  `vkUpdateDescriptorSets` on the frames before the palette first resolves.
* **`s_sharpInk` was never reset** where `s_sharpOn` is reset on every call, so a session that once
  had coverage and then took an early return published `sharpOn = 1` with no quads for ever — which
  this pass reads as "the GL lane has coverage I did not produce" and stops compositing.
* Two smaller: the layer was recorded **above** the composite gate, so it was drawn and discarded on
  every frame the composite was stood down on (every frame of a Classic++ session); and the two new
  refusals said nothing in the log.

**And the re-review found two more of mine, both in the fixes above.** The refusal logs it asked for
printed **at the frame rate**, because `s_saidSharp = 0` was the first statement of the branch that
then tested `if (!s_saidSharp)` — the latch never held, so a condition holding every frame printed a
line every present. And **the budget refund counted frames on which the store is provably not
level**: while `s_behind` stands the replay applies *nothing*, so those are exactly the frames that
prove nothing, and counting them let a pass waiting for a fresh start that never comes refund its
budget for ever and go on asking the **oracle** for a reseed with the mute unable to latch. That is
the **third** version of the same mistake on this pass, and the comment beside it already said what
to check.

One was mine and not theirs: publishing the engine pair through `mir_bytes` made a **latent**
staleness reachable — `mir_finish` had already taken `arena` and `alen` further up, so a realloc
there would have left every op's `aoff` pointing into freed heap. Re-published after the copy.

**An overflowing list is a `compose = 0` and NOT the behind state**, and that distinction is the
one the twins established the hard way: a sharp-layer quad mutates nothing that outlives its frame,
so a frame the pass cannot draw is *one frame*, never a store out of step. Nothing is asked of the
producer for it.

#### A set is claimed for one image for the length of a frame

A descriptor set may not be rewritten once a recorded draw names it, and the twin **array** cannot
be the index: `tw_drop` moves the last entry into the hole, so the indices shuffle under it. The
claim is on the **view** instead, and `SET_MAX` bounds how many a frame can want. There are exactly
three kinds of image a twin draw samples — the one UI atlas (every sprite), the one glyph atlas
(every string) and a copy's **source twin** — so the bound is **the twins plus two**.

**It was the twins plus ONE until landing 2, and the string op is what made that wrong.** The old
comment's argument was "every sprite samples the one atlas", which stopped being true the moment
there were two non-twin images.

**AND `TW_MAX + 2` IS NOT A BOUND EITHER — IT IS A SIZE.** The landing review split on this and the
dissenting reviewer is right: `tw_drop` **deliberately** leaves a claim standing (clearing it was a
previous round's fix and caused a worse hazard — a set a recorded draw already names being
rewritten), so what a frame counts against `SET_MAX` is **distinct views claimed**, not twins alive.
A present that batches FREE + SEED + COPY churn claims a view per *generation* of a surface, and
nothing bounds that by `TW_MAX`. What IS by construction is the consequence: `set_claim` answers 0
deterministically, the replay goes to `standdown`, and `behind()` drops **our** store and asks the
producer once. Nothing is corrupted and nothing of the GL lane's is touched. Sized for the case
that has to work, degrading predictably past it — stated here rather than dressed up as a bound.

#### Two things the measurement found, and both were the port's

**`s_sharpOn` MEANS "THE LAYER EXISTS", NOT "ANYTHING IS IN IT"** — and it exists on every frame
once it has been made. The pass refused on it, so with `nocursor nominimap nostring` armed it stood
down on **every** frame while every counter that would explain why read zero. An empty layer and a
disabled one composite identically (it is taken only where its alpha says it has coverage), so the
port may draw a frame whose layer is empty and must refuse one whose layer is not. `s_sharpInk` is
that question, answered from the two clients' own counters.

**`DRAW_MAX` WAS SIZED FROM A FLIP AND THE HAND-OVER CARRIES A PUBLISH.** 4096 came from "a steady
screen is ~41 ops of which a handful draw", which is true of a flip; the publisher batches every
flip since its last one and the shell flips ~12 000 times a second at a 5 ms cadence. Measured at
**7414** on a 1080p level load.

#### Measured

**LANDING 1, MEASURED 2026-09-16** on the binary the LANDING REVIEW's fixes produced (every figure
re-taken after them), `gui.on=nostring nocursor nominimap norestore`, `vk.on=color=0,0,0`, Classic,
reference setup's 4070. **Each capture's own size is what confirms its fixture** — see the trap in
`ta-drive`:

| | |
|---|---|
| shell `MAINMENU`, 640×480, two runs | **0 of 307 200**, 306 737 ink a side |
| `selbox-slope` in game, 1024×768 | **0 of 786 432**, 748 890 ink a side |
| `selbox-slope` in game, 1920×1080, two runs | **0 of 2 073 600**, 1 989 791 ink a side |
| one earlier shell run's two files | **byte-identical**, which is the strongest form of a 0 |
| the behind state | **fired, asked for the fresh start, and recovered to 0 px** |
| the twin store doing the work | 2 twins; millions of sprite ops, tens of thousands of copies |

So the op stream, the seeds, the pixel ops, the sprite quads through the resolved atlas rects, the
copies through `CPY_FS`, the palette and the engine's own frame beneath all reproduce the GL twin
exactly.

**LANDING 2, MEASURED 2026-09-16 WITH STRINGS ON** — `gui.on=nocursor nominimap norestore`, and the
absence of `nostring` is the whole point of the table. Same binary discipline, same lever, same
fixture:

Re-taken on the binary the **re-review's** fixes produced, every figure:

| | |
|---|---|
| `selbox-slope` in game, 1024×768 | **0 of 786 432**, `str=12/48 miss=0 reseed=0 repack=0 glyphs=27 fonts=1`, `mirlost=0` |
| `selbox-slope` in game, 1920×1080 | **0 of 2 073 600**, `str=6/25`, `mirlost=0` |
| the capability refusal | **gone** — no stand-down line at all, where landing 1 fired one on every in-game frame |
| the behind state | fired twice, both at the shell→game transition, both recovered, no mute |
| what those two events were | `an op names a surface this store never seeded` and the pixel-op form of it — the holes the re-review found, which used to be reported as the presented surface having no twin |
| the states the fixes added | **none fired**: no lost record, no ask cap reached, no quad bound, no dimension refusal |

**LANDING 3, MEASURED 2026-09-16 WITH THE SHARP LAYER ON** — `gui.on=norestore mmbase`, and the
absence of `nocursor` and `nominimap` is the point of the table. `mmbase` forces the sharp minimap
at `k = 1`, which it would otherwise leave to the engine. Same fixture, same discipline:

| | |
|---|---|
| `selbox-slope` in game, 1024×768 | **0 of 786 432**, `sharp=1024x768` |
| `selbox-slope` in game, 1920×1080 | **0 of 2 073 600**, `sharp=1920x1080` |
| the cursor | `curs=1,21x23,dev=0,sc=1.00,drawn=20990` and `11388` — it is on screen and drawn, not absent |
| the minimap | `mm=10810` and `2559` |
| the strings, still | `str=7/30 miss=0 reseed=0 repack=0 glyphs=27 fonts=1`, `mirlost=0` |
| the sharp stand-down | **gone** — where landing 2 fired it on every frame with a cursor on screen |

**THE COUNTERS ARE THE CHECK THAT THIS 0 IS NOT VACUOUS, not the ink figure** — the lesson landing
2 wrote two screens up. A layer the Vulkan lane drew *nothing* into, while the GL lane drew a
21×23 cursor and a minimap, would differ by thousands of pixels and not by none. `drawn=` and `mm=`
are what say both lanes had something to compare.

**AND ONE PATH INSIDE THE MINIMAP IS STILL NOT COVERED BY IT, WHICH THE REVIEW FOUND AND THE
COUNTER CONFIRMS.** `MM_FS` has two outcomes: where the engine's 3×3 neighbourhood is *fogged or
overdrawn* it takes the engine's own texel through the palette, and only where that neighbourhood
is **clean** does it sample our `uPic` picture. `fog=13251/13356` and `13250/13356` on the two runs
above — **99.2 % of the engine's minimap is fogged**, so the clean test almost never passes and the
picture is sampled almost nowhere. `200v200`, whose units are spread across the map, reads
`13137/13356` and is no better. So these figures test the **mask** thoroughly and say **nothing**
about the picture.

That matters because the picture is exactly where the two lanes' *filters* differ — see the sampler
finding below. **A fixture with a substantially explored map is what would close it**, and none of
the ones here is that.

**So `norestore` is the only lever left** — and landing 4 below takes it, after which the pass
composites an ordinary session with nothing armed for it at all. Through landings 1 and 2 it never
did: landing 1 stood down on every frame with text on screen, and landing 2 on every frame with a
cursor on screen, which is every frame of real play.

So TA's own glyphs — rasterised by the GL lane into its own atlas, stamped from the cells that lane
resolved, coloured by the blitter's own three arguments — reproduce it exactly.

**THE CHECK THAT THE 0 IS NOT VACUOUS IS `str=`, NOT THE INK FIGURE.** A first draft of this section
argued from the ink count moving between landing 1's run and landing 2's (748 890 against 744 960)
and that argument is **wrong**: re-running the identical fixture gives 748 916 and 1 995 067, so the
figure moves between runs of the same configuration. The cause is on record — **the engine's
`PAUSED` banner BLINKS**, ~2 800 px at 1024×768 (§2.33's traps), which is the size of the swing.
Both halves of one A/B are the same frame by construction, so a 0 stands whatever the phase; what
says the strings were actually drawn is the producer's own counter, `str=38/102` with
`glyphs=27 fonts=1` — 38 string ops, 102 glyph quads. Read the counter, not the ink.

**The last row is the honest one and it is the shape of this whole pass.** The glyph serial, the
repack loss, the abandoned-frame publish, the ask cap, the quad bound and the `SET_MAX` size are all
**correct by construction and exercised by nothing here** — the fixture's glyph set is complete
before the first upload, its atlas never repacks, its store holds two twins, and its worst frame is
7414 quads. They are in the same category as §2.33's Classic++ refusal: written, argued, unfired.

**Every defect this pass has had was invisible to every measurement taken of it** — the
use-after-free, the five diverging paths, the reseed storm, the `SET_MAX` size, the glyph serial,
the repack, the withheld record, the one-shot reseed a lost frame could swallow, and three
`continue`s that had been diverging quietly since landing 1 — and every one was found by
**reading**: five reviewers across three rounds, and two passes of reading my own diff. On a pass
whose output is a pixel count that reads 0, that is what to expect rather than the exception.

**Three of those were introduced by a FIX for one of the others**, which is the real lesson of this
landing and the reason each round was re-reviewed rather than trusted: clearing `s_setView` in
`tw_drop` (worse than the hazard it closed), `behind()` reseeding the oracle every frame, and the
ask cap whose refund an ordinary session could never reach. Twice the evidence against my own fix
was already written down on this page.

**One PLAUSIBLE finding was kept rather than fixed.** GL's `uSurf` is POT-padded by the fork
(1024×512 for a 640×480 mode) while the Vulkan `uSurf` image is exactly the mode rect, so a `texelFetch` outside
that rect reads a defined texel in GL and an undefined one here. It cannot reach the A/B — the
composite samples inside the viewport — and closing it would mean padding our image to match a fork
detail rather than an engine one. Named rather than silently carried.

**BOUNDED, not padded, by landing 10 (§2.62).** The exposure was never the padding: it was that
`LAY_FS` clamps its fetch to `uSize - 1` — the *presented twin's* size — and then `texelFetch`es
`uSurf`, which is sized by the *primary*. A primary smaller than the twin reads out of range
whatever the padding is. Landing 10 had both numbers in hand for the first time and now refuses
the case: `engW < h.surfW || engH < h.surfH` composites with the guard disarmed instead of
sampling. The GL half of this paragraph is history — that lane went in 4d-1/4d-2 — and the symbol
it used to name (`s_engImg`) no longer exists; the image is borrowed from `tagpu_vk_surf.c`.

#### Landing 4 — Classic++, and the question the plan could not answer from the plan

**THE PLAN SAID THIS LANDING MIGHT BE BLOCKED ON THE RESTORER'S FIVE SHADERS. IT IS NOT, AND THE
REASON GENERALISES.** `SPR_FS` is the only producer of colour in this module and it samples
`uAtlasRGB`, the UI atlas's **restored twin** — which the Classic++ restorer paints on the GPU
(`tagpu_restoreglsl.c`, renderers.md 4c) out of five shaders G19c did not translate. Reading that as
"the port needs the restorer" is the mistake: **a second backend needs the TEXELS, never their
producer.** The restorer goes on running exactly once, in the GL context, and its output crosses as
bytes like every other input on this hand-over.

So `tagpu_gaf.c` grows a second opt-in mirror beside the indexed one, and it is deliberately **not**
the same mechanism:

| | the indexed mirror (landing 1) | the restored mirror (landing 4) |
|---|---|---|
| where the bytes come from | the CPU already has them — the decode that feeds GL feeds the mirror | only the GPU has them; a **read-back** is the only way |
| when it is written | at every paint, beside the `glTexSubImage2D` | on a frame `tagpu_rglsl_job_painted` moved, and on no other |
| arming it | marks every painted entry for repaint, or the mirror holds zeros where art is | nothing: the destination is cleared to **alpha 0** when the job is made, and alpha 0 is the restorer's own "not painted here", so a mirror allocated at any moment is behind but never wrong |
| what it costs a settled session | nothing | **nothing** — the UI atlas's restore finishes and `painted` stops moving |

**The content key is the paint count, `rgbGen` for its discontinuity, and the row bound for the
shelf.** All three are things that change what a consumer would read and nothing else is: keying on
the paint count alone misses a **re-arm** (the job is freed and counts from 0 again), and keying on
those two misses a shelf that **grew** without a paint having landed in it yet. This is landing 2's
lesson at a different atlas — a serial that is not the CONTENT's serial uploads once and then misses
everything after it — applied before it cost a round rather than after.

**The read-back sits between `tagpu_rglsl_step` and the drain**, which is where `tagpu_gui_present`
already says, in its own comment, "what it paints this frame is what the drain's sprites sample". So
the bytes handed over are the bytes the GL lane's own draws read, on the same frame — not a frame
behind them. `glReadPixels` from an FBO over the twin gives memory row 0 first (the attachment is a
texture, whose y = 0 is memory row 0, not the screen's bottom row), so there is no flip here either.

##### What the ops carry, for the fourth time on this pass

`TAGPU_GUIOP::col` is two bits of what the GL lane **did**:

* `TAGPU_GUICOL_DST` — the destination twin has a colour attachment after this op. `twin_colour`
  can refuse (no `glDrawBuffers`/`glClearBufferfv`, an incomplete FBO), so "the op wanted colour"
  and "the twin has colour" are two facts and both are needed.
* `TAGPU_GUICOL_ON` — the program's own `uRestored` (sprite) or `uSrcHasCol` (copy) was 1.

Deriving either here would be asking `s_colValid && s_atlas.rgb` a second time, of a module that
settled it in `restore_step` **before the drain**. Same rule as the sprite's atlas rect, the
string's glyph cells and the cursor's destination — and the same reason each time.

##### Where Vulkan is not GL

* **A framebuffer is immutable.** GL flips `glDrawBuffers` between 1 and 2 on one live FBO; here a
  twin that gains colour gains a **second framebuffer** over both views and a second render pass
  with two attachments. Everything else about that pass is identical — same `LOAD`, same
  dependencies — because it is the same draws writing one more output.
* **No shader was translated.** `SPR_FS`, `CPY_FS` and `STR_FS` all already declare
  `layout(location=1) out`, and landings 1–3 ran them against a **one-attachment** pass: a write to
  a location the subpass has no attachment for is discarded, which is exactly what
  `glDrawBuffers(1)` does over there. The colour edition is the same SPIR-V against a pass that has
  the attachment.
* **`twin_col_drop` is a render pass of its own.** `vkCmdClearAttachments` is the only rect clear
  Vulkan has and it needs an instance; GL does it with a scissored `glClearBufferfv` on buffer 1.
  For the same reason the CLEAR op now clears **both** attachments on a colour twin —
  `glClear(GL_COLOR_BUFFER_BIT)` clears every buffer `glDrawBuffers` named.
* **The descriptor claim is on the PAIR.** Binding 41 stopped being the dummy the moment Classic++
  arrived — a sprite samples the restored atlas there and a copy its source's colour twin — so
  keying the claim on binding 40 alone would hand a restored sprite the set of an unrestored one.
  Right indices, wrong colour image, and a picture that is **correct everywhere the colour twin
  happens to be empty**. `SET_MAX` gains two for the same reason.
* **The retire takes two object sets in ONE entry.** `fb2` names both views, so split across
  entries the sweep could destroy an attachment's view while the framebuffer naming it was alive.

##### Five places the lanes could have diverged in silence, refused instead

Every one of them writes a twin the **next** frame inherits, which is why none is a best-effort
draw: a restored sprite before the restored atlas has crossed (reachable for exactly one frame —
the pass asks for the mirror from inside its own `prepare`, so the present that ARMS the restore can
run before anything has asked); a copy whose source has colour over there and not here; the
presented twin the same way; the composite's own `uColOn`; and a `twin_colour` this lane could not
honour. The first is answered with a **fresh start** and that is not a formality — though the
mechanism is stronger than the first draft of this paragraph said, which claimed "a reseed
re-publishes every surface's bytes, and `twin_col_drop` then clears the colour on both sides".
Traced by the review: `behind()` raises `g_guiq.reseed`, the producer emits **`PK_RESET`**, and
`twins_reset` → `twin_drop` **deletes** each colour twin outright (`glDeleteTextures(1, &t->rgb)`)
while `tw_reset` retires ours. The colour is not cleared, it ceases to exist on both sides, which is
a stronger guarantee than the one this page was resting on.

And the palette re-arm — `restore_step` freeing the job and invalidating every colour twin —
is applied on a **sweep** over the store rather than at each twin's next op. A twin nothing touches
this frame is still one the composite may present.

##### THE DEFECT, AND IT TOOK A SECOND RESOLUTION TO SEE

`tagpu_rglsl_job_new` **clears its destination** when the job is made (`prepare_dest`), so every
texel of the GL restored twin above the shelf reads alpha 0. Ours is only ever written for the rows
the mirror has read back, and the rest of a 2048 square was **whatever the allocator handed us** —
which `SPR_FS` reads as restored colour wherever a byte of it clears 0.5 alpha, writes into a colour
twin, and the composite then shows.

| the same build, the same fixture, the same levers | |
|---|---|
| `selbox-slope`, 1024×768 | **0 of 786 432** |
| `selbox-slope`, 1920×1080 | **166 827 of 2 073 600** |

**The garbage was always there and only sometimes read.** The run that measured 0 had not restored
anything yet — `GL(restore)` and `GL(norestore)` were byte-identical on it — so no sprite took the
`uRestored` branch at all. One resolution, taken alone, would have called this landing done. The fix
is `vkCmdClearColorImage` over the whole image on creation, which is the GL lane's own behaviour
rather than a guard added around it.

##### Measured, with the restore ARMED and SETTLED

`gui.on=mmbase` — **no `norestore`, and no STAND-DOWN lever is left on this pass.** `mmbase` is not
one: it is a force-ON for the harness (`s_mmforce`, "draw it at k = 1 too"), which makes the GL lane
draw the sharp minimap at a scale where it would otherwise leave it to the engine, and so gives
*both* lanes more to compare rather than less. Every token that made the GL lane draw LESS is gone.
[The distinction is the review's: the sentence above the table read as a contradiction to the line
beside it.]
`tagpu_classicpp.on` is in `tagpu_opt.c`'s play-defaults table with `assets=1`, so this is what an
**ordinary session** does, and landings 1–3 stood down on every frame of one.

| | |
|---|---|
| `selbox-slope` in game, 1024×768 | **0 of 786 432** |
| `selbox-slope` in game, 1920×1080 | **0 of 2 073 600** |
| the same two with `norestore`, after the fix | **0** and **0** — landings 1–3 unregressed |
| the counters at 1024×768 | `colvalid=1 col=19/2 rgb=11 atlas=40/4096`, `sprites=847765 copies=5130 str=38/102 drawn=35090` |

**AND THE NON-VACUITY CHECK IS A THIRD CAPTURE, not a counter.** §2.34 has twice had to correct an
argument from ink counts, so this one is a picture-to-picture diff in the **GL lane alone**: the same
frame with `norestore` and without.

| what dropping `norestore` moves in the GL lane's own picture | |
|---|---|
| 1024×768 | **68 598 px of 786 432** — 8.7 % of the frame |
| 1920×1080 | **220 372 px of 2 073 600** — 10.6 % |

That is what the Vulkan lane reproduced to the pixel, and it is a claim no counter could make. It is
also the check that caught the defect above: the run where this number was **0** was the run whose
A/B was meaningless.

##### What the review and the RE-review found, and where they landed

Two reviewers at `high`, then one re-review of the fixes. **No CONFIRMED defect in the
synchronisation, the lifetime, the retire or the content key** — the arguments this section makes
about those all held when traced. What they did find:

* **The creation clear closed UNDEFINED memory and left STALE memory open.** `s_arRows` was a
  monotone high-water reset only on a dimension change, and `mirrorRgbRows` drops to 0 in two places
  that change no dimension. An upload after a shrink covered the new rows and left the previous
  fill's colours above them. A high-water records that rows were once written, never that they still
  say the right thing — which is exactly how it hid, and the 166 827 px above is the proof those
  rows get sampled.
* **THE UI ATLAS'S GENERATION WAS NEVER CHECKED ACROSS A PRESENT, AND THAT HAD BEEN OPEN SINCE
  LANDING 1.** `tagpu_gaf_atlas_put` runs INSIDE the drain, so a sprite that fills the atlas recycles
  it in the middle of the very present whose earlier sprites are already recorded with the rects it
  resolved. Landing 2 built precisely this guard for the GLYPH atlas; the sprite atlas was left
  without one for three landings. The subtlety that makes it safe is that `twins_reset` recycles the
  atlas too — so *every* RESET moves the generation, and a guard that did not forget the ops before
  one would lose every present answering a reseed and answer it with another.
* **`SET_MAX` stopped bounding its worst case the moment the claim became a pair** — a copy SOURCE
  can be claimed twice in one frame because `uSrcHasCol` is a property of the DESTINATION. Both
  reviewers, independently; the eighth such pair on this lane. `2 * TW_MAX + 3` is exact rather than
  slack: 64 copy claims, 2 sprite, 1 glyph.
* **And the RE-REVIEW found a defect inside a fix, for the third round running on this pass.** The
  whole-image clear and the row copy are two TRANSFER writes to overlapping memory recorded back to
  back, and **recording order is not execution order**: without a barrier between them the driver may
  land the clear after the copy, and every restored sprite then draws indexed against a `uRestored`
  that says otherwise, silently, for as long as the serial stands still. The fix for the stale rows
  is what made that path routine — it used to run once per image.

Three documentation claims were disproved and corrected: a fresh start DESTROYS the colour twin
rather than clearing it (the conclusion was right, the mechanism named was not); "no lever is left"
meant no STAND-DOWN lever, beside a table measured with a force-ON one; and this page's own
producer-side comment still asserted that rows past the shelf "can change no texel any op samples",
which is precisely the premise the 166 827 px killed. A fourth was a safety claim with no mechanism
behind it — the cursor's generation mark cannot fire, because `atlas_insert` answers exhaustion with
a flag and a NULL and never touches the generation.

**AND THIS SECTION WAS WRITTEN INTO §2.32 THE FIRST TIME, which is landing 1's bug wearing different
clothes.** That one put §2.34 itself below the `## 3. Known limits` header; this one dropped 128
lines about the UI pass into the middle of the shadow map's, because the anchor it was inserted
before — `#### Not covered` — occurs in three sections and the first match is not this one. Both
were caught by the same thing and by nothing else: **regenerating the wiki and reading the headings
of the page you just wrote.** A note that renders in the wrong section is not documentation, and
`git diff` shows it as an addition in the right file.

#### Landing 5 — the context switch, and the crash that was hiding behind it

**THE SWITCH IS MEASURED IN ONE PROCESS, BOTH DIRECTIONS, AND IT IS 0 PX AT EVERY STOP.** Every
figure above this was taken after a `--restart`, which starts a process, crosses the shell→game
switch on the way in, and measures the far side. This one drives the menus by hand
(`ui click SINGLE` → `Skirmish` → `Start`, and back out through `EXIT` → `MAINMENU` → `CHOICE1`)
so that both sides of both crossings are the same process:

| | |
|---|---|
| the shell, 640×480, Classic++ armed | **0 of 307 200** |
| → the switch: lane down, window destroyed, new window, new swapchain, **up in 173 ms**, one fresh start | |
| in game, 1920×1080 | **0 of 2 073 600** |
| → the switch back: down, destroyed, created, swapchain at 640×480, **up in 145 ms** | |
| the shell again, after the round trip | **0 of 307 200** |

**THE WHOLE-LANE TEARDOWN IS CORRECT AND NOT A COST TO REMOVE.** `render_ogl.c`'s render loop calls
`tagpu_vk_render_stop` on every exit, and its comment is the argument: a mode change invalidates the
HWND the Vulkan surface was made on, so the lane comes down on the thread that owns it rather than
being left to discover a dead window. This is also what the ta-drive skill measured for the world
passes in G19e ("an in-process map change brings the WHOLE Vulkan lane down and back up").

##### And the reverse crossing CRASHED, in the GL publisher, with nothing to do with this port

Quitting a skirmish to the main menu at 1920×1080 took an access violation and the process then
spun at 100 % CPU with no further output. The route is the one the skill documents as the way to do
it, and `MAINMENU.GUI 640x480` is its own stated criterion for the level having really torn down.

**IT REPRODUCES WITH `vk.on` REMOVED, and the fault is identical** — same instruction pointer, same
call stack, same illegal-read shape. That is what makes it the fork's rather than the lane's, and it
was worth an extra run to establish before touching anything.

**Finding it took one step past what TA's own crash handler says.** The handler prints "in module
TotalA.exe", but that image ends at `0x51FC00` and the IP was `0x7903BBA1`; `/proc/<pid>/maps` on
the still-spinning process puts that page on **our own `ddraw.dll`**, so it is RVA `0x3BBA1`.
Disassembling there gives `cmp BYTE PTR [edi+0x9], 0` — the exact bytes the ErrorLog printed — with
`0x811C9DC5`, this function's own FNV basis, two instructions later.

The function is `frame_key` in `tagpu_gui_hook.c`, and the defect is one line wide:

```c
if (!ptr_ok(px)) return NULL;      /* the PIXEL pointer is checked */
if (fr[0x09] == 0) {               /* the FRAME HEADER is not, and never was */
```

`fr[0x09]` is `TAGPU_GF_COMP`, and the per-level GAF bank it lives in had just been freed by the
teardown's cascade (`freed 279 block(s)`). Every other caller of the GAF resolvers in this tree —
`tagpu_fx.c`, `tagpu_feat.c`, `tagpu_render3do.c`, `tagpu_gui_surf.c` — puts the header through
`tagpu_gaf_frame_sane` first.

##### The fix is an ordering, and the obvious cheaper key does not work

A `frame_sane` on `fr` **would** have stopped this crash, because the page is unmapped. It is still
not the fix: a freed-but-still-mapped page passes `IsBadReadPtr` and returns garbage, which is the
same bug with better odds and wrong art instead of a stop. It is kept as a **bound**, and this page
says plainly that it is not the safety argument.

The safety argument is that the op carries **the level it was OBSERVED in**, and `publish` refuses
to resolve a GAF frame while a teardown is in flight, from a level that has ended, or when nothing
is tracking levels at all.

**THE "ONE THREAD" PART IS ESTABLISHED BY THE CODE, AND A PREVIOUS REVISION OF THIS PARAGRAPH SAID
IT WAS NOT.** It claimed `before_flip` checks `on_game_thread()` while "the blit observers that call
`op_add` do not". They all do: `tagpu_gui_leaves.h` gates every entry point that can reach `op_add`
— `before_gaf`/`gafa`/`gafb` (:102,103,106), text (:121), line (:177), `bar`/`frame`/`rect`/`focus`
(:202,205,206,208), copy (:219), gafd (:249), scale (:270), fill (:292) — and `on_game_thread()` is
`s_gameTid != 0 && GetCurrentThreadId() == s_gameTid` with `s_gameTid` set in `tagpu_gui_init`, so it
is never vacuously true. `publish` itself runs only from `before_flip`, past the same check. The
stamp and the bump are on one thread, by construction, and this is program order rather than a claim
about visibility. [The false version was caught by BOTH reviewers of the third pass; a note that
invents a hole is worse than a missing one, because the next session goes and "fixes" it.]

**THE GENERATION IS `tagpu_packet_pub`'s, AND KEYING IT ON `tagpu_reclaim`'s WAS THE FIRST VERSION'S
REAL DEFECT.** Reclaim's generation moves only while reclaim is ARMED — and `tagpu_reclaim.off`, one
file, leaves it a constant `0` while the engine frees the GAF banks exactly as before, because the
cascade is the ENGINE's and reclaim defers only `Object3do` and model templates. The gate was
therefore inert in precisely the configuration that needs it, with `frame_sane` — the probe this
section says is not the safety argument — left as the only thing between the shipped DLL and the
crash. **Both landing-5 reviewers found it independently**, the ninth such pair on this lane.

The tree had already ruled on this class and the fix used the counter that does not move.
`tagpu_packet_pub` installs its **own** observer on the teardown `0x491B60` when reclaim is not
armed, for the stated reason that the level-end packet must not depend on another module being
armed, and `tagpu_packet_pub_level_gen`'s own header says it is what "a game-thread observer that
latches per-level state" should stamp with. `tagpu_packet_pub_level_tracked()` is the first test:
when no provider exists the generation never moves, and no ordering being available is a reason to
**refuse** the read rather than to take it.

**AND THAT PREDICATE WAS WRONG THE FIRST TIME, WHICH IS THE FOURTH LANDING RUNNING WHERE THE
RE-REVIEW FOUND A DEFECT INSIDE A FIX.** It read `s_levelEndBy != 0`, which is assigned only when the
PUBLISHER is armed — so under `tagpu_packet.off` it answered "nothing tracks levels" while the
generation was moving perfectly well, because `tagpu_packet_pub_level_end` bumps it as its first
statement, above every gate, and reclaim's wrap calls that from a hard-wired stub whether this
module is armed or not. The gate would then have refused **every** GAF op for the whole session and
degraded every UI sprite to a box upload — fail-safe, and a total silent loss of the sprite path on
the one lane whose cost is about to be measured. It is `s_levelEndBy != 0 ||
tagpu_reclaim_level_tracked()` now: either provider moving the counter is enough.

One asymmetry between the two providers is worth knowing and is **not** closed: reclaim's wrap is an
unconditional stub, so it bumps the generation on whatever thread the teardown runs on, while this
module's own observer returns early off the game thread and the generation then does **not** move
for that teardown — which fails OPEN at the gate. It is only reachable if `0x491B60` can run off the
game thread, which the review could not establish either way.

**AND THE WINDOW THIS SITE ACTUALLY SEES IS NOT THE ONE THE FIRST DRAFT DREW.** It argued that the
teardown flag covers the frees and the generation covers what follows, so neither is sufficient
alone. That is true of the flag and the generation in general and **false of this call site**:
`publish` runs only from `before_flip`, and `s_teardown` is non-zero only while that same thread is
inside `0x491B60`, which does not flip — so the closing test is unreachable from here today. The
case that is real is the **opposite** order, and it is why the generation does the work: at
`0x460635` the engine calls the teardown and pops the screen **afterwards**, flag already down, and
those ops are refused because their generation is stale. The closing test stays as belt — a flip
reached from inside a teardown would be caught by it — and is named as belt rather than as the
argument. [The reviewer traced all six `call 0x491b60` sites to establish this.]

The fallback when any test refuses is the op's own box out of **our mirror of the surface**, not out
of the asset — so the picture is unchanged and only the sprite's identity is lost.

**THE COUNTER IS THE SHAPE OF THE FIX; IT IS NOT EVIDENCE THAT THE FIX WORKS.** Two versions of this
paragraph thought otherwise. The first argued from `lost=` being flat — but `lost=` is the CONSUMER's
atlas miss, and a publisher-side refusal emits `PK_PIXELS` and never a `PK_SPRITE`, so it could not
have moved whatever happened. The second argued from `gafstale=` alone, which says how often the gate
fired and nothing about whether the crash it is aimed at would have happened. **The fault is
intermittent, so on this route every run that does not crash looks like a fix**, and a clean run
after a change is worth nothing until the rate of the thing being prevented is known. That rate was
never measured, and that — not the counter — is the real defect in how this landing was first argued.

**SO IT WAS MEASURED, ON THREE BINARIES.** The same route five times per arm — `SINGLE` → `Skirmish`
→ `Start`, into the game, then `tab tab` → `EXIT` → `MAINMENU` → `y`, with the shell's 640×480 read
back as the proof that the level really tore down — at 1920×1080 with the **Vulkan lane DOWN**, which
is the configuration the crash was first seen in:

| arm | the `frame_sane` bound | the ordering | crashed |
|---|---|---|---|
| **A** — neither, i.e. the landing-4 tip | out | out | **4 of 5** |
| **B** — the ordering ALONE | out | **in** | **0 of 8** |
| **C** — what ships | **in** | **in** | **0 of 5** |

**Arm B is the one that carries the claim, and it is why the bound is not the argument**: with
`frame_sane` compiled OUT and only the ordering standing, the gate refused the reads at the teardown
and nothing crashed in **eight** runs, against a base rate of four in five — `0.2^8 ≈ 3×10⁻⁶` if the
ordering did nothing. It was run to eight rather than five for a second reason: with no bound in the
build, arm B is also the probe for the SCREEN-POP window below, which the gate provably does not
cover.

**Arm A's four crashes are one fault, not four.** Every one of them: `80 7f 09 00` at the
instruction pointer — `cmp byte ptr [edi+9], 0`, which IS `fr[0x09] == 0` — at the same EIP, with
the faulting address exactly `EDI+9` and reading `0x09A48D21` every time, because the per-level bank
comes back at a fixed address and only the timing of the race varies.

And the fifth arm-A run, the clean one, has an explanation rather than being noise: on arm C the
counter reads `gafstale=0` on one run in five as well, and **a run with nothing straddling the
boundary is a run that would not have crashed unfixed either.** Two fractions of one-in-five
agreeing is n=1 against n=1 and is called a coincidence worth noting, NOT a positive control — the
third review pass asked for that word back and was right to. What carries weight is arm B: 0 of 3
against a base rate of 0.8 is p ≈ 0.008 on its own.

The counts differ between arms — arm B refused 147 where arm C refuses 215 or 225 — and that is
run-to-run spread, not a difference between the builds: arm C alone ranges 0 to 225 across its five.
How many ops are straddling the boundary depends on where in the census window the teardown lands.

The shape inside one arm-C run, which is what the counter is actually good for:

| | |
|---|---|
| in play | **`gafstale=0`** on 30 heartbeats while `sprites=` climbs 9 831 → 534 525 |
| `packet: level end -> gen 1 (reclaim's 1)` | 6462 in-play draws that level |
| after the teardown | **`gafstale=215`** on 18 heartbeats — and **no other value all session** |

Across the five arm-C runs: 215, 225, 0, 225, 215. It steps once, at the boundary, and those are
reads that used to go into freed memory.

**THE TRAP THAT COST A ROUND OF THIS: THE ARMS WERE FIRST RUN WITH THE VULKAN LANE UP, AND ARM A
READ CLEAN 1 OF 1 THAT WAY.** Route D draws a second window's worth of work in the same iteration of
`ogl_render`, so the publisher's queue drains on a different schedule and the race closes. A crash
reproduction has to run in the configuration the crash was seen in, and "the lane is off" is part of
that configuration even when the bug has nothing to do with the lane. On the strength of those clean
runs this section had already withdrawn the `225` above as an artefact of the `s_levelEndBy` defect.
**That withdrawal was wrong** — the number reproduces at 215/225 on the corrected predicate, because
in the configuration it was measured in the publisher was armed and that disjunct never fired. The
re-review's finding stands as a latent defect under `tagpu_packet.off`; the measurement it appeared
to discredit was sound.

**THE SAME GATE NOW STANDS IN FRONT OF THE FONT, AND IT SHIPS UNMEASURED ON THE PATH IT GUARDS.**
`publish`'s `OP_TEXT` branch handed `op->frame` — the FONT object here, not a GAF frame — straight to
`gfont_slot`, which reads `f[3]` and `f[0]` with no bound, no probe and no generation, on a pointer
captured up to `CENSUS_MS` earlier. The identical recorded-before / published-after shape the rest of
this function had just closed, twelve lines below it, and **both reviewers of the third pass found it
independently** — the tenth such pair on this lane. The font is `[globals+0x204]` (`0x4B6220` is
`mov eax,ds:0x51FBD0`), so it is probably not per-level; "probably" is not a lifetime. It is gated
the same way rather than a different way, because a different rule would need an argument the font's
lifetime does not give us, and a refusal falls through to the box's own bytes — what this path did
before G17d, so the picture is unchanged. The refusal is counted as **`strstale=`** and NOT folded
into `gafstale=`, because the A/B above is stated in that counter and a second reason inside it would
make those numbers mean something else on the next run that reads them. **Both counters are gone
since**: `gafstale` with landing 7 and `strstale` with landing 8, each when the gate it counted
stopped existing — the figures quoted in this section belong to the builds they were measured on.

**What could not be measured, and it is a gap that predates this gate.** The consumer's `str=`
counter — strings actually drawn and mirrored — read **0/0 on every fixture driven for this landing**:
in game at 1920×1080 under `gui.on=mmbase` and under landing 2's own `gui.on=nocursor nominimap
norestore`, and in the shell on `SKIRMISH.GUI` at 640×480. The A/B reads 0 px in all of them, and **0
px over content that is not there is not a measurement** — the lesson landing 1 already paid for. The
gate is not the cause: the control build with it reverted reads the same `0/0`, and `strstale=0` says
it refused nothing. So the change is established to make no difference and is NOT established to be
correct on a live string.

**A FIXTURE THAT DOES PUBLISH STRINGS EXISTS SINCE, and it is the walk** [MEASURED 2026-09-16,
landing 8]: the in-game `--vk` walk reads **`str=` 6 281–6 532 strings / 30 291–31 537 glyph
quads**, `miss=0`, `fonts=1`, 27 glyph cells. Launching a skirmish and letting it sit still reads
`str=0/0` with the same levers and `cpp=1` either way, so it is **what the walk drives** and not
the arming; which of the walk's actions reaches `DrawTextCustomFont 0x4C14F0` is not established
and does not need to be for the figure to be usable.

**AND `glyphs=` CLIMBING DOES NOT MEAN A STRING WAS DRAWN**, which is the trap that made the
sitting fixture look like it had no strings at all. In that run `glyphs=` reached 16 cells while
`str=` stayed 0 — and `glyph_raster` has exactly one caller, `tagpu_text_glyph_feed`, so the
string ops *did* arrive. The feed sits in the drain loop **above** the skip gate and runs whatever
the switch decides; the stamping is `case PK_STRING`, which needs `twin_find(o->surf)` to find a
twin and needs the op not to have been skipped to a reset. `miss=`, `reseed=` and the
"stamped nothing" line were all 0, so `twin_string` was never entered. Read `str=` for "text was
drawn" and `glyphs=`/`fonts=` for "the records arrived"; they are different questions.

**AND THAT PUTS A QUESTION MARK ON LANDING 2's OWN NON-VACUITY.** Landing 2 reports "0 of 786 432 at
1024×768 and 0 of 2 073 600 at 1920×1080, with strings ON" on that same lever set, and the run here
at 1080p on that lever set published no string at all. The map and the instance differ, so this is a
question and not a refutation — but the next work on this pass needs a fixture that DEMONSTRABLY
publishes one (`str=` climbing, not merely `nostring` being absent) before any string figure on this
page is believed, this gate's included. The only session today that read a non-zero `str=` had walked
`RENDER.GUI` and `VISUALS.GUI`, which are our OWN injected menus and a different text route.

**This is the standing debt in `CLAUDE.md` coming due, once.** The roadmap already lists "the
per-LEVEL ASSET class under `tagpu_reclaim`'s fence" as an open row of the frame-packet gate; this
landing closes the one site of it that the UI publisher owns, by ordering rather than by waiting for
the asset channel, and leaves the class itself open.

#### Not covered

* **The gate, which is the whole of G19f and not these landings.** `uiwalk`'s `strict` walk over
  the **full screen inventory** at 1024×768 and 1080p, shell and in game; and **frame time no worse
  than GL**, which nothing in Phase G has measured at all. The shell↔game context switch is closed
  **for the crash that was measured** — the teardown cascade — and the SCREEN-POP window inside the
  same route is open, covered only by the bound, and written up below. These are a handful of fixtures, not an inventory.
* **THE PER-LEVEL ASSET CLASS ITSELF STAYS OPEN.** Landing 5 closes the one site of it the UI
  publisher owns, by ordering. The class — model templates, FeatureDef and wreck records, the GAF
  banks, all under `tagpu_reclaim`'s fence until the asset channel — is the frame-packet gate's own
  row and nothing here touches it. The only reason this one was found is that a fixture happened to
  quit a skirmish to the main menu. The landing-5 review swept the rest and **most are sound** — the
  feature, unit, terrain, effects and packet-publisher readers all sit behind the level-end packet's
  own `in_game` gate, which is published before the flag drops — but it named **two that are not**:
* **THE SCREEN POP IS THREE INSTRUCTIONS INSIDE THE ROUTE THIS LANDING MEASURED, AND THE GATE DOES
  NOT COVER IT.** This bullet used to file `GUI_Pop 0x4A9660` away as "the shell's route", which is
  true and misleading. The disassembly of the site the landing itself cites:

  ```
  460630:  call 0x491b60     ; the teardown -- bumps gen N->N+1, lowers s_teardown
  460635:  push 0x1
  460637:  call 0x491d70     ; UpdateIngameGUI -- DRAWS, so op_add stamps lgen = N+1
  460641:  add  eax, 0x519
  460647:  call 0x4a9660     ; GUI_Pop -- frees that screen's art
  ```

  The pop happens **after** the bump, so ops recorded at `0x460637` carry the CURRENT generation.
  At the next flip `level_tracked()` is 1, `level_closing()` is 0 and `op->lgen == gen()` — **the
  gate passes**, and the only thing between `frame_key` and freed art is `frame_sane`, the probe
  this section says twice is not the safety argument. `GUI_Pop` frees from **39** call sites with no
  flag and no generation, and `frame_key`'s own comment has recorded for some time that the shell
  hands those same addresses to the next screen. (The "21 event-handler call sites" this bullet
  carried until the re-review is `UpdateIngameGUI 0x491D70`'s figure, borrowed for the wrong
  function; `call 0x4a9660` appears 39 times in the pristine exe and `call 0x491d70` 21.)

  **Arm B is the experiment that says whether it is live, and it says probably not — which is not
  the same as no.** Arm B is the build with `frame_sane` compiled OUT and only the ordering standing,
  so an unguarded read faults instead of being swallowed. **8 runs of the route, 0 crashes**, against
  arm A's 4-in-5. If the ops recorded at `0x460637` named art the pop frees, arm B would have taken
  the access violation. So on THIS route, with THIS fixture, those ops do not appear to name popped
  art — evidence, not proof, and it says nothing about the shell's own screen-to-screen pops, which
  no fixture here drives at all.

  **The by-design fix exists and is not built.** The module already observes the engine's single
  free — `before_memfree` on `MEM_Free 0x4D85A0`, which every one of the engine's 363 free sites
  calls, on the game thread, at the entry before the allocator's own critical section — and already
  uses it to retire surface-table entries by block. A block-keyed forget for queued ops would cover
  the teardown cascade and the pop together and would not need a generation at all. What it needs
  first is a size: `MEM_Free` is handed the block, not its length, so "is this op's frame inside
  this block" is not answerable from the observer as it stands. [Raised by the third review pass,
  2026-09-16. Named here rather than built because it is a mechanism, not a fix to this landing's
  defect, and it wants its own measurement.]

* **`tagpu_render3do.c`'s unit atlas is never dropped at a level boundary.** Its entries key on the
  template's texture-frame ADDRESS, and the atlas resets only when full or on a context loss —
  where `tagpu_fx.c` and `tagpu_feat.c` both call `tagpu_gaf_atlas_forget` for exactly the recycled-
  address reason. A second level handed the same address would be served the first level's texels,
  and nothing would detect it. Not this landing's to fix, and it is wrong art rather than a crash.
  (The GL UI atlas is safe here: `publish` stores a CONTENT hash as the identity, so a recycled
  address cannot alias.)
* ~~The cursor, the minimap and the sharp layer~~ **CLOSED by landing 3.** `norestore` is the only
  lever left.
* **THE MINIMAP'S PICTURE PATH (`uPic`), and with it the only LINEAR sampler in this module.** The
  GL texture is `MIN_FILTER = GL_LINEAR, MAG_FILTER = GL_NEAREST` and the Vulkan lane matches it
  with a second sampler — **which took two attempts, the first being a no-op** (see above) — but no
  fixture here reaches that branch of `MM_FS` (99.2 % fogged), so the match is **correct by
  construction and unmeasured**. That combination is exactly how the first attempt survived: nothing
  the pass measures would have changed had the sampler stayed wrong. It is the same category as §2.33's
  Classic++ refusal. Closing it needs a fixture with an explored map.
* **The sharp layer's THIRD client, the sharp string path**, is not exercised by any fixture here:
  `SHARP_TEXT` draws at device resolution where `twin_string` stamps into the twin, and nothing in
  `selbox-slope` takes it. The quads would cross like any other, but that is an argument and not a
  measurement.
* ~~Classic++ colour twins and the MRT sprite/copy programs~~ **CLOSED by landing 4**, and it was
  **not** blocked on the restorer's five shaders: they go on running in the GL context and their
  output crosses as bytes. **No lever is left on this pass.**
* **THE RESTORE'S OWN COST, which this landing pays and does not measure.** The read-back is a
  `glReadPixels` of `2048 × (shelfY + shelfH) × 4` bytes on every frame the restorer painted, and it
  **synchronises** — it is issued right after the paint draws. A settled session pays nothing (the
  UI atlas's queue drains and `painted` stops moving) and an unarmed one pays nothing at all, but
  the frames DURING a fill are not counted anywhere and no figure here bounds them. It belongs to
  landing 6 with the rest of the cost question.
* **THE READ-BACK CHANGES THE ORACLE LANE'S TIMING, AND "THE GL LANE IS UNCHANGED" IS THEREFORE NOT
  LITERALLY TRUE WHILE IT IS ARMED.** `glReadPixels` is synchronous, and the restorer slices its
  work against a GPU-time budget (`tagpu_restoreglsl.c`, `budget=MS`) — so with the mirror armed the
  GL lane paints a different number of batches per frame than without it. It changes no PIXEL, and
  the lane is unarmed in every session that is not running this A/B, but the claim this page makes
  everywhere else is about the lane's OUTPUT and this is the one place the distinction matters.
  [The review's, and it is the honest form of the sentence.]
* **THE UI ATLAS'S GENERATION GUARD IS UNFIRED HERE.** `mirlost=0` on every run above, and
  `atlas=53/4096` — these fixtures never fill or recycle the sprite atlas, so the frame-loss path
  the review opened is correct by construction and exercised by nothing. It is in the same category
  as the glyph atlas's own repack guard, which landing 2 added and no fixture has ever taken either.
* **A PALETTE RE-ARM IS NOT EXERCISED BY THESE FIXTURES BEYOND ITS FIRST.** `rearms=1` on every run
  — the one the shell→game transition causes — so the sweep that invalidates every colour twin has
  fired once and always with two twins in the store. The Gamma slider and `+gamma N` are what drive
  it in play and neither is in a fixture here.
* **The present itself** — landing 5. Route D still gives the Vulkan lane a window of its own.
* **Frame time**, which is half the gate's own wording — landing 6, and nothing in Phase G has
  measured cost at all.
* **The glyph atlas at dimensions this pass will not carry.** `ATLAS_MAXDIM` bounds the upload, and
  a glyph atlas outside it leaves `s_glHave` 0, which takes the replay to `standdown` and the store
  to a fresh start on every frame that draws a string. `GA_W`/`GA_H` are compile-time **512 × 256**
  in `tagpu_text.c` against an `ATLAS_MAXDIM` of **8192**, so this is a refusal that has never been
  exercised and cannot be without editing the GL lane — the same category as the Classic++ one
  in §2.33.
* `ss` 2, the owed teardown, an in-process map change, and the validation layer still.

### Landing 6 — the frame-time harness, and why its ratio does not answer the gate

`tagpu_ftime.c/.h`, armed by `tagpu_ftime.on`, inert without it. Two GPU timestamps per lane per
frame, a 256-frame ring each, p50 and p99 rather than a mean, nothing blocks: the GL side polls
`GL_QUERY_RESULT_AVAILABLE` and carries a frame whose pair is not ready, the Vulkan side is read
behind the fence the seam already waits on before it re-records that slot.

**TIMESTAMPS ARE FORCED, NOT PREFERRED.** `GL_TIME_ELAPSED` is scoped, only one may be active per
target, and `tagpu_restoreglsl.c` already runs one around every restorer slice — a frame bracket of
that kind would either fail to begin or break the restorer's. `glQueryCounter` has no such rule and
is what the Vulkan half does anyway (`vkCmdWriteTimestamp`).

**THE HEADLINE THIS SECTION FIRST CARRIED — "the Vulkan lane's frame costs 0.56–0.66 of the GL
lane's, the gate met with margin" — IS WITHDRAWN.** It was measured, it was reproducible within a
run, and it was wrong about what it measured. What follows is what the harness actually establishes,
which is less than the gate wants and more useful than nothing.

#### What it measures: an ELAPSED SPAN, and the two spans are not the same

`glQueryCounter` records when the GPU **reaches that point in the command stream**, so the delta is
elapsed time on the GPU timeline and counts anything that stalls inside the bracket — the render
thread's own `EnterCriticalSection` three lines after `gl_begin`, the restorer's synchronising
`glReadPixels` — whether the GPU was working or idle. The two brackets then span different things:
GL's runs the whole CPU frame (top of `ogl_render` to just before `tagpu_vk_frame`), Vulkan's only
its own command buffer (`TOP_OF_PIPE` after `vkBeginCommandBuffer` to `BOTTOM_OF_PIPE` before
`vkEndCommandBuffer`). That difference is survivable only on a frame where the GPU is saturated
throughout. It is not survivable here.

#### The measurement that withdrew the claim

One binary, one fixture (`200v200`, `--maxfps 0`), the sim **paused at a fixed tick** (~950–975,
`alive` 381–387), the pause verified by peeking `*0x511DE8+0x38A47:4` twice:

| resolution | MP | `gl p50` | `vk p50` | **vk/gl p50** | fps |
|---|---|---|---|---|---|
| 640×480 | 0.31 | 5.501 ms | **0.052 ms** | **0.010** | 166.6 |
| 1280×720 | 0.92 | 156.650 ms | 93.966 ms | **0.600** | 6.2 |
| 1920×1080 | 2.07 | 118.840 ms | 105.281 ms | **0.886** | 7.8 |

**THE RATIO IS NOT A PROPERTY OF THE LANES.** It moves 0.010 → 0.886 across resolutions on the same
binary and the same paused scene, and it moved 0.556 → 0.886 at 1920×1080 between two builds that
differ only by this landing's review fixes. A figure that swings by 90× is not "what a Vulkan frame
costs relative to a GL frame".

**`vk p50` CANNOT BE THE LANE'S OWN WORK.** 52 µs at 640×480 against 105 ms at 1920×1080 is a factor
of ~2000 for 6.75× the pixels. 52 µs is a believable command-buffer cost for this scene on this GPU;
105 ms is not. What the bracket picks up at the higher resolutions is the command buffer **waiting
for a GPU the GL lane is saturating** — route D runs both lanes in one iteration of `ogl_render`, so
they contend, and `TOP_OF_PIPE`→`BOTTOM_OF_PIPE` spans a queue stall exactly as it spans work.

**AND THE COST IS NON-MONOTONIC IN RESOLUTION.** 1280×720 is about twice as slow as 1920×1080 with
2.25× FEWER pixels, reproduced on one binary. This is an unexplained anomaly of the GL lane and is
worth its own investigation; it is flagged here and not diagnosed. It also disposes of the argument
an earlier revision of this section made — that 6.75× fewer pixels giving 10.7× the frame rate
established a fill-bound frame. Those were two points on a curve that does not run that way, and the
landing-6 review had already challenged the inference on principle (a resolution-DEPENDENT CPU cost
produces the same scaling with the GPU idle) before the third point showed the premise was not
merely unproven but false.

#### What the harness is still good for

* Within one paused scene it is precise: run A's absolute figures drifted 13 % between two
  consecutive report windows while the ratio repeated to three decimals.
* It is the first instrument on this lane that reports per-frame GPU-timeline cost at all, and it
  found the 720p anomaly on its third run.
* The GL half under-samples at high frame rates and this matters when reading it: at 166 fps it
  harvested **296 of ~5300 frames** against the Vulkan half's 4850, because `GLQ` is 8 pairs and a
  frame that finds none free is skipped. The two lanes' percentiles are then computed over very
  different samples of one session, and the GL sample is selected for frames where the driver had
  caught up.

#### Not covered

* **THE GATE'S FRAME-TIME CLAUSE IS NOT ANSWERED and goes back to OPEN.** Answering it needs a method
  that does not put both lanes on one GPU in one iteration — each lane measured alone in its own run,
  or per-pass timing rather than per-frame. That is a decision about what the measurement IS, not a
  fix to this code.
* **THE 1280×720 ANOMALY IS NOT DIAGNOSED.** ~2× slower than 1080p at 2.25× fewer pixels, on the GL
  lane, on this fixture.
* **THE RESTORE'S READ-BACK IS STILL NOT SEPARATELY MEASURED.** It is a synchronising `glReadPixels`
  inside the GL bracket and a candidate for both the absolute figures and the anomaly.
* One fixture, one map, one scene, paused.

### Landing 7 — the two holes the landing-5 sweep named, closed by moving the read

Landing 5's review swept the module for the per-level asset class and named two sites it could
not close. This landing closes both. Neither is about Vulkan: they are defects in the shipped GL
renderer that the port's fixtures found.

#### The sprite: resolved where the engine proves it alive

**The publisher took a sprite's identity hash AND its decoded plane out of engine memory at
publish time**, up to `CENSUS_MS` after the blit that recorded the op. Two engine routes free
that memory inside the window:

* the level teardown's cascade — which landing 5 closed with a generation ordering, at the cost
  of refusing 215 ops at a measured level end;
* **`GUI_Pop 0x4A9660`**, which frees a popped screen's art from **39** call sites with no flag
  and no generation, and which landing 5 could **not** close. The disassembly is why:

  ```
  460630:  call 0x491b60          ; the teardown -- bumps gen N->N+1, lowers s_teardown
  460635:  push 0x1
  460637:  call 0x491d70          ; UpdateIngameGUI -- DRAWS, so op_add stamps lgen = N+1
  46063c:  mov  eax, ds:0x511de8
  460641:  add  eax, 0x519
  460646:  push eax
  460647:  call 0x4a9660          ; GUI_Pop -- frees that screen's art
  ```

  The pop is **five** instructions after the bump, so ops recorded at `0x460637` carry the
  current generation and the gate passes. Only `frame_sane` stood there, and a bound is not the
  safety argument. (**This listing said "three instructions" and silently omitted `0x46063c` and
  `0x460646` until both landing reviewers counted them, independently.** The count changes
  nothing about the argument — the pop is after the bump either way — but a listing that reads
  as contiguous and is not is exactly the kind of note that costs the next person an hour.
  `call 0x4a9660` appears **39** times in the pristine `.text`, `0x460647` among them; re-counted
  with the correction.)

**Both reads moved into `gaf_box`, the `before_` detour on the blit leaf**, and the ordering is on
the engine's timeline:

> our read  <  the engine's blit  <  the engine's free

Every free route that opened this window — the cascade, and all 39 pop sites — runs **after** the
blit it follows, and the caller holds the art alive across the call it is making. Our read precedes
that call. It needs no flag, no generation and no `MEM_Free` block size, and it covers free routes
nobody has found yet, because it does not enumerate them: it is earlier than all of them. After it,
`publish` dereferences **no engine asset memory on this path at all** — the `gui probe:` trace
included, which was the last one left. `frame`/`pix` survive in the op as the consumer's atlas KEY,
a value compared against a table and never followed.

**It is NOT the claim that "the engine would fault if this were dead".** This section said that
until the landing review; the detour runs *before* the engine's read, so if the memory were dead
**we** would fault first. That was a counterfactual dressed as a proof, and it is not what makes
the change safe.

**AND IT BOUNDS LIFETIME, NOT EXTENT.** The engine reads the **clipped sub-rect**;
`tagpu_gaf_decode` reads all `w*h`, or every RLE row. A header whose `w`/`h` exceed the plane the
loader actually allocated is therefore covered by nothing above — only by `tagpu_gaf_frame_sane`,
a SHAPE test (`w,h <= 512`), and the decoder's own `IsBadReadPtr`, which this note says everywhere
is not a safety argument. **That residual is unchanged by this landing**: the same decode read the
same bytes at publish before it, over memory that might *also* have been freed. Moving it removes
the lifetime half and leaves the extent half exactly where it was. Bounding it needs the plane's
allocated length, and the plane is not a block start, so `MEM_Size` cannot answer it either.

**It also fixes a wrong-art case the generation could not see.** The key is a hash of the plane
precisely because the shell hands a freed screen's addresses to the next screen's art.
Taken at publish time, that hash read whatever the address held *then* — so art freed and replaced
inside one census window hashed the **new** content under the **old** op, and the consumer matched
a key naming pixels the op never drew. Taken in the observer, it is a hash of the bytes the engine
is about to blit, which is the only content the op ever meant. Nothing measures this case; it is
an argument from what the two versions read, and it is stated as one.

**The generation gate is removed from this path** rather than kept as belt. It guarded the two
reads that moved, could never cover the pop, and was not free. `gafstale` went with it — the name
changed with the meaning on purpose, because `gafstale=215` is the figure landing 5's A/B is
stated in and a counter that keeps its name while measuring something else is how those numbers
quietly stop meaning what these notes say they mean. **The `OP_TEXT` path kept its gate and its
`strstale` through this landing**, `gfont_slot`, `glyph_block_size` and `glyph_block_fill` still
reading the font object at publish, and `op->lgen` existing for them alone — **closed the same
way one landing later; see Landing 8 below.**

#### The UI atlas: dropped at the level boundary too

**Found by the landing review — both reviewers, independently, the eleventh such pair on this
lane.** `tagpu_gui_surf.c`'s UI atlas matches entries on `(o->frame, o->pix, fw, fh)` — the
frame's **address** and its content hash — and its only resets are `twins_reset`, the atlas
filling, and a GL context loss. **None of those is a level boundary.** The engine frees a level's
GAF banks and the next level's loader may hand a new frame an old one's address; `frame_key`
hashed only the plane's first 64 bytes plus the hotspot then (the whole plane since
[gui-renderer](../gui-renderer.html) §30), so **UI art whose first RLE row is one
transparent run collided by CONSTRUCTION**, not by 2^-32 luck — and then `atlas_find` hits the
old entry and the twin draws the previous level's texels, with no counter moving.

**The gate this landing removed was never the cover for that**, which is worth saying plainly
because it is the obvious thing to assume: `op->lgen != level_gen` refused ops **recorded** before
a boundary and **resolved** after one — a ~5 ms window — and did nothing whatever about entries
already sitting in the consumer's atlas from the previous level. Those survived it. So the hole
predates this landing and the fix is a **drop**, not a refusal.

The publisher raises a reseed when the level generation moves, and the machinery is already
there: `PK_RESET` makes the consumer call `twins_reset`, which calls `tagpu_gaf_atlas_reset`, and
the UI atlas never asks for a repack, so that goes straight to `atlas_drop` — every entry, every
hash. Same shape as the unit atlas's, one level down the stack.

#### The unit atlas: dropped at the level boundary

`tagpu_render3do.c`'s atlas matches entries on the frame header's **address** and the pixel
plane's (`tagpu_gaf_atlas_find`), so an entry is right only while that address means that art. The
engine's teardown frees the model textures and the next level's loader may hand a new frame an old
one's address — at which point the atlas serves the **previous level's texels** and nothing
detects it: the entry is valid, the UV is in range, the picture is simply wrong. It reset only
when FULL or on a GL context loss, and neither is a level boundary. `tagpu_fx.c` and
`tagpu_feat.c` both already drop theirs here for exactly this reason.

**This was live in ordinary play**, not only under a lever: `tagpu_native.on` is in
`tagpu_opt.c`'s play-defaults table as `"all wrecks"`, so every play session has the unit atlas
up, and any session that played a second level was exposed.

`tagpu_r3d_atlas_level` is called from `tagpu_native_frame` beside `cache_gen_check` and **before**
`tagpu_posebake_frame`, not from `tagpu_r3d_atlas_frame` lower down the file: posebake **latches**
`tagpu_r3d_atlas_gen()` for the whole frame, so a drop after it would stamp this frame's bakes
with the generation before the drop and cost a second, pointless drop on the next frame. Taken
where it is, every consumer sees one generation per frame.

#### What was measured

| claim | how | result |
|---|---|---|
Everything below is from the binary that lands — the whole set was re-run after the review's
fixes, because those fixes changed the DLL and a figure off an earlier build names a build that no
longer exists.

| claim | how | result |
|---|---|---|
| the picture does not move | the A/B walk, all three resolutions | **52 of 52 stops at 0 px**; the 1024×768 and 1080p walks are identical to the pre-change runs **stop for stop, to the byte** |
| the crash route is clean | the landing-5 route, 5x at 1920×1080, **Vulkan lane DOWN** | **5 of 5 clean**, `teardowns=1` each |
| the scratch bound holds | `gafscratch=high/lost/baddec` | high-water **860 849 bytes of 2 097 152 (41 %)**, **`lost` 0, `baddec` 0** |
| nothing falls back for a real reason | `gafnoplane` | **0**, over every run |
| **the UI atlas** drops at each boundary | two skirmishes in ONE process, `gui.on=...log` | **`gui: reset #3: level-changed` and `#6`** — exactly two, one per level end |
| **the unit atlas** drops at each boundary | the same session | **2 resets**, logged `subject replaced` (our call, not a full-atlas recycle), each one line after the UI atlas's |
| a dropped atlas re-decodes the RIGHT texels | `glshot` on level 2, after the drop | units render with their own textures and shadows; terrain, trees and HUD intact |

**One stop of the 52 is not reproducible between runs and it is not this landing's.** `ARMMAIN2`
at 640×480 — the first in-game stop, the one closest to the scenario load — read 103 153 px of
ink in one run and 103 376 in the next, a 223 px difference. The same stop is **identical across
all three 1024×768 runs** (118 946 every time), so it is not the review's fixes, and both lanes
agreed exactly in both runs, so it says nothing about either. The cause is **not established**;
it is recorded rather than explained, because the ink column is evidence about what the engine
drew and an unexplained wobble in it is worth a line even when the diff it guards reads 0.
**[SETTLED as far as it can be, 2026-09-16, landing 8]**: the same walk run TWICE on one binary
reads 103 074 and then 103 376, so the wobble is the fixture and no binary difference produces
it. The cause is still unknown; the question "is it the change?" is closed.

`gafreseed` is counted apart from `gafnoplane` and is **not** a failure: a reset clears the seen
table after an op has already decided it needs no plane. It is **cheap rather than free** — the
word this section used until the review corrected it: each one costs a `PK_PIXELS` box in the
arena and one window without its atlas identity, in a publish that is already re-seeding every
surface whole, so nothing is on screen that would not have been, and it self-heals next window. It read **4 323** over the three
walks. One counter for both would have read as 3923 failures on the first
session that measured it — which is exactly what it did read before the split.

#### Not covered

* **THE FONT WINDOW WAS STILL OPEN AFTER THIS LANDING** — the `OP_TEXT` path still
  dereferenced the font object at publish, covered by the level generation and by `ptr_ok`, neither
  of which is a lifetime. It was named here as an open window rather than left to look closed by
  the line above it in the source, and **Landing 8 closed it** by the same move.
* **The wrong-art case the move fixes is an argument, not a measurement.** It needs a pop and a
  reload inside one ~5 ms census window, and no fixture here forces that.
* **The A/B cannot see this class at all**, and that is worth saying plainly: both lanes consume
  the same published op stream, so a publisher that resolved the wrong art would hand both lanes
  the same wrong art and score 0 px. The walk is the regression gate for this landing, never its
  evidence.
* **The unit atlas's drop is evidenced by the log line and a picture, not by a diff against the
  bug.** Forcing the address reuse the fix exists for would need the second level's loader to be
  handed a specific block, which nothing here can arrange.
* `MEM_Size 0x4D8360` exists and would have made the block-keyed forget buildable; it was not
  used because it reads the heap outside the allocator's own critical section. See
  [exe-reverse-engineering](../exe-reverse-engineering.html).

### Landing 8 — the font window, closed the same way

Landing 7 moved the sprite's two reads into the observer and said, in the source and here, that
the `OP_TEXT` path still read the font object at publish and that a level generation was standing
in for a lifetime it did not have. **This closes that**, by the same move, and it is the last
per-level engine asset `publish` dereferenced. Like landing 7 it is not about Vulkan: it is a
defect in the shipped GL renderer that the port's fixtures found.

#### What moved

`gfont_slot` and the walk that turns a string's unsent codes into glyph records now run in
`before_text` — the detour at the head of `0x4CCF60`, the engine's glyph blitter — with the
engine's own `font` and `str` arguments in hand. The op carries what the publisher needs and
nothing it would have to follow: `fid` (the slot id the consumer's glyph cache keys on), `gboff`/
`gblen` (the records, in a 128 KB window scratch beside the sprite's 2 MB one), `frows`/`fyoff`
(the two header bytes the consumer stamps quads with) and `fgen` (below). `publish` copies our own
bytes out of our own scratch.

**Two publish-time walks became one observe-time walk.** `glyph_block_size` sized the block and
`glyph_block_fill` wrote it; with a scratch that can be bounded per record, sizing first buys
nothing. `glyph_block_capture` writes, and `glyph_block_mark` — which reads no font, only our own
records — marks them sent at publish.

#### The ordering, and why it is a stronger argument here than on the sprite

The detour sits at the head of the blitter with its arguments, one instruction before it walks
that string through that font, so

> our read < the engine's read < any free of the font

holds by the engine's own sequencing. **It is not "the engine would fault if this were dead"** —
the detour runs first, so we would fault first; that phrasing was a counterfactual dressed as a
proof when the landing-7 review found it on the sprite path and it is no better here. What makes
the read safe is that the engine has already committed to making it.

**And it bounds EXTENT as well as lifetime, which the sprite's move did not.** `0x4CCF60` has no
clip and no destination bound at all — it writes `sum(widths) × font[0]` pixels wherever the
caller said, and it cannot skip a glyph's bits because the destination would be off-screen. Our
walk takes the blitter's own two skips (`sub ebx,first; jb` at `0x4CCFAA`, `or ebx,ebx; je` at
`0x4CCFB9`) and reads the same bytes for a **subset** of the string's codes — the ones this font
has not sent yet. So where landing 7 closed the lifetime half and left the extent half exactly
where it found it (the engine blits a clipped sub-rect; `tagpu_gaf_decode` reads all `w×h`), this
path has no extent half to leave. The instruction-level read set is in
[exe-reverse-engineering](../exe-reverse-engineering.html), "WHAT THE BLITTER READS, EXACTLY".

#### What the gate cost, and what replaced it

**Nothing replaced it.** `op->lgen` is gone from the op and from `op_add`, `strstale` is gone, and
`tagpu_packet_pub_level_tracked` and `tagpu_reclaim_level_closing` now have **no caller in the
tree** — kept rather than deleted, because they are the level-lifetime API those modules expose
and removing them is not this landing's business.

**What did have to be added is a generation, and it is not the level's.** The block omits the
codes the font has already sent, and *that* decision is the only thing between the capture and the
flip that can go stale. Two things clear a `sent[]` table: the render thread throwing its glyph
atlas away (a shelf overflow or a ninth font, `gfont_check_gen`), and a publisher reseed skipping
whole windows. Until this landing both were safe **by position** — the decision was taken inside
`publish`, after either clear had already happened in the same call. Now both go through
`gfont_sent_clear`, which bumps `s_sentGen`; the op stamps it at capture and `publish` publishes
its box instead on a mismatch. Same shape, and the same reason, as the sprite path's `s_seenGen`,
and counted apart as `strrearm=` for the same reason `gafreseed` is.

A **slot recycle** needs no bump of its own, but **not** for the reason this section first gave
("nothing in flight is invalidated"), which the cross-thread reviewer showed is backwards. Both
tables are eight deep, so the ninth `(font, sig)` that makes the producer recycle is also the
ninth id the consumer sees, and `tagpu_text.c`'s own `gfont_slot` answers that with
`memset(s_gf)` + `memset(s_gatlas)` + `s_ggen++` — every cell under every old id, gone. A recycle
therefore *reliably causes* a consumer clear. What makes it safe is the generation catching that
`s_ggen++` like any other, and `gfont=` now prints recycles and resends side by side so the two
moving together is a cross-check rather than a hope.

**And the poll sits beside the decision, not at the top of `publish`.** The first version of this
landing called `gfont_check_gen()` once on entry and claimed that made the test "a comparison
against NOW". It made it a comparison against the top of the publish loop, and the render thread
can drop its atlas in the middle of one: op *k*'s block omits a code because `sent[]` said it was
published, the consumer overflows its shelf and clears, and op *k* is then committed without it —
one window of a string drawn with that character dropped and the rest closed up, `miss=` counting
it. Before G19f-8 the poll was inside `gfont_slot`, one statement before the decision it guards,
so that first version had *widened* an existing window rather than closed one. It is now polled
per op, which restores exactly the old width. [Found by the cross-thread reviewer, with the
interleaving spelled out.]

#### Marked at publish, not at capture — and no cross-op dedup

`sent[]` is set by `glyph_block_mark`, after `pub_bytes` has taken the arena slot and the bytes are
in it. Marking in the capture would mark glyphs that a queue overflow, an arena overflow, an
untwinned surface or a `dedup()` drop then threw away — and a glyph marked sent but never sent is
missing from every string for the rest of the session, which is the exact failure `gfont_check_gen`
was added to prevent.

That is also why **two ops in one window that need the same unsent code each carry it**. The
sprite path can share one decode through `s_gcap` because its consumer keys on the frame and one
copy serves every op; a glyph record is only ever read out of the op that carries it, and either op
may be the one that does not get there. The duplicate is idempotent at the consumer
(`glyph_raster` returns early on a known cell) and costs arena bytes in first-sight windows only.

#### The cost, measured

`OP` grew from **80 to 96 bytes**, so `s_ops[65536]` grew from 5.24 MB to 6.29 MB; with the 128 KB
scratch the DLL's `.bss` goes from **42 163 060 to 43 343 252 bytes (+1 180 192, +2.8 %)** on a
module that already reserves 42 MB of it. Read off `i686-w64-mingw32-objdump -h` on the two builds.

#### What was measured

Everything below is from the binary that lands — the whole set was re-run after the review's
fixes, because those fixes changed the DLL and a figure off an earlier build names a build that no
longer exists.

| claim | how | result |
|---|---|---|
| the picture does not move | the A/B walk, three resolutions, the 640×480 one run TWICE | **78 of 78 stops at 0 px** (2 × 26 shell + in game at 640×480, 13 in game at 1024×768, 13 at 1920×1080) |
| …and not merely at 0 px | the ink column against the pre-change binary | **1024×768 and 1080p identical STOP FOR STOP, TO THE BYTE**; the 640×480 exception is below and is the fixture |
| the string path is actually live | `str=` | **26 788 string ops / 129 288 glyph quads** over the four walks, `fonts=1`, 27 glyph cells |
| no glyph goes missing | `miss=`, `reseed=`, `repack=`, the "stamped nothing" log line | **0 of each**, over all of it |
| **the re-arm guard fires, and costs nothing** | `strrearm=` | **6** at 1080p (0 on the other three): six text ops published their box because `sent[]` was re-armed between their capture and their flip — and `miss=` stayed **0**. The same six appeared on the pre-review binary, so it is the walk's own 1080p shelf pressure and not a flake |
| the scratch bound holds | `glyscratch=high/lost` | high-water **7 632 bytes of 131 072 (5.8 %)**, `lost` **0** on every run |
| nothing regressed in the sprite half | `gafnoplane` / `gaflost` / `gafbaddec` | **0 / 0 / 0** |
| the arena takes the duplicates | `overflows=` | **0** — the per-op blocks a first-sight window now duplicates cost arena bytes and overflowed nothing |
| the crash route is clean | the landing-5 route, 5× at 1920×1080, **Vulkan lane DOWN** | **5 of 5 clean**, `teardowns=1` each |
| landing 7 still holds | two skirmishes in ONE process | `gui: reset #3: level-changed` and `#6`, and the unit atlas's **2** `subject replaced` resets (generations 4 and 9) |

**`strrearm=6` is the line to read twice.** It is the only new failure mode this landing creates —
a block that omits codes because they were "already sent", published after something cleared that
table — and it is why the generation exists rather than being argued away. Six of those happened
in each 1080p walk, all six published their box instead, and no glyph was missed. Without the
stamp those six strings would have drawn with characters dropped and the rest closed up, which is
the failure `gfont_check_gen` was added for in the first place.

**THE 640×480 WOBBLE IS THE FIXTURE, AND THAT IS NOW ESTABLISHED RATHER THAN ASSUMED.** Landing 7
recorded `ARMMAIN2` at 640×480 as varying between runs and could not say why. Running that walk
**twice on this one binary** settles the question the only way it can be settled: `ARMMAIN2` read
**103 074** in the first and **103 376** in the second — the same two values the pre-change and
post-change runs had produced, so a figure that moves between two runs of one build cannot have
been caused by the change. `ARMCOM1` reads 93 688 in three of the four runs on record and 94 312
in one (on the pre-review binary), which is what `ARMCOM1-back` — the same screen visited again —
reads in all four. Both lanes agree to the pixel at every one of them, and both stops are
identical to the control at 1024×768. The CAUSE is still not established; what is established is
that it is not this landing, and not landing 7's fixes either.

#### Not covered

* **The A/B cannot see this class at all**, exactly as in landing 7: both lanes consume the same
  published ops, so a publisher that resolved the wrong glyphs would hand both the same wrong
  glyphs and score 0 px. The walk is this landing's **regression gate**, never its evidence. What
  is evidence here is `miss=` — a consumer-side count of glyphs the cache refused that the engine
  would have drawn — and the ink column against the control.
* **The wrong-font case the move fixes is an argument, not a measurement.** A font freed and
  reloaded at the same address inside one ~5 ms census window would, before this, have had its
  new header read under the old op; nothing here forces that, and the shell — where address
  recycling is routine — draws no strings at all.
* **THE SHELL DRAWS NO STRINGS**, which bounds what the 26 shell stops say about this landing:
  `0x4CCF60` is reached only through `DrawTextCustomFont 0x4C14F0`, so `glyscratch` is 0 across
  the whole shell half and those rows are a regression gate on the rest of the module. Everything
  this landing changes is in game.
* **A STRING LONGER THAN 256 BYTES IS TRUNCATED BY US AND NOT BY THE ENGINE** [named by the
  landing review, 2026-09-16]. `0x4CCF60`'s walk has no counter — it draws to the NUL or the
  `'\n'`, however long that is — while `before_text` measures the op's box from the first 256
  widths and `twin_string` stamps at most 256 quads. So a longer string would draw short in the
  twin AND have a box too narrow for the pixel fallback to cover. It predates this landing on
  both sides and nothing here changes it; it is named because the note this landing adds to the
  engine map originally attributed our 256 to the engine, which would have hidden it. No fixture
  draws one: TA's HUD strings are short.
* **THE WINDOW BETWEEN THE CHECK AND THE CONSUMER'S DRAW CANNOT BE CLOSED FROM THE PRODUCER, and
  is not.** The generation makes the "already sent" half of a block true as of the moment the op
  is committed; the render thread may still throw its glyph atlas away between that instant and
  the drain, and then that one string draws with the missing characters dropped. It is bounded by
  a window, self-heals on the next publish, and `miss=` counts it — 0 over 19 585 string ops here.
  **The reviewer's proposed tightening was rejected after verification**: raising `g_guiq.reseed`
  when `twin_string` records a miss would make the losing window the last one, but `miss` is not a
  divergence signal — a string containing any code the font has no glyph for increments it while
  the engine skips that code too, which `tagpu_gui_surf.c`'s own comment says at the counter. That
  would re-seed every surface on an ordinary string.
* **A font whose header lies is refused exactly as before and no better** — `f[3] != 0`,
  `f[0] == 0` and `gfont_glyph`'s zero tests. A table entry pointing outside the loaded file
  image, or a width byte that runs the bits past its end, is read by us and then by the engine one
  instruction later; this landing neither adds nor removes that.
* **The level gate that was removed had never fired on any measured fixture** (`strstale=0` over
  the control's whole walk), because every string is in game where `level_tracked()` is 1. So the
  change is established to move no pixel, and its value is the lifetime argument rather than a
  behaviour it corrects.
* **`tagpu_packet_pub_level_tracked` and `tagpu_reclaim_level_closing` now have no caller.** They
  are kept as those modules' level API; deleting them is a separate decision.

### The gate's walk — the Vulkan lane against the GL lane, over the whole inventory

`tools/uiwalk.py --vk`.

**WHAT THE GATE ASKED FOR IS NOT WHAT WAS RUN, and the substitution is the first thing to say.**
The gate's wording is a **`strict`** walk over the full screen inventory at both resolutions, shell
and in game, with the Vulkan lane matching what the GL lane scores. `strict` is `uiwalk`'s existing
mode and it means something specific: it diffs **our** frame against the **engine's own surface**
and counts the holes. That is a parity oracle against the engine, and **it is not a valid
regression with Classic++ on** (see above — Classic++ is deliberately not the engine's output), so
it cannot answer a question about the Vulkan lane at all. `uiwalk` also had no Vulkan support
whatever before this landing.

What was run instead is an **A/B of the two lanes against each other**: same process, same frame,
same UI ops, GL writes one capture and Vulkan writes the other, and the two are diffed pixel for
pixel. It answers *"does the Vulkan layer put the same pixels on the screen as the GL layer"*,
which is the question the gate is about. It does **not** answer *"are those pixels right"* — that
is `strict`'s question and both lanes could be wrong together and still score 0. The GL layer's own
parity against the engine is G15's evidence (§2.3e), measured under `norestore` where the oracle is
valid, and this walk inherits it rather than re-establishing it.

`--vk` arms the lane in its own window — `gui.on=mmbase classicpp.on vk.on=color=0,0,0`, **no
`norestore`**, so landing 4's restored UI atlas is live and is what the comparison runs through —
and at every stop re-arms `tagpu_gui.ab`, waits for both lanes to write, and diffs the pair with
`tools/vk-ab.py --pass gui`. The lever is one-shot per arming (`s_abDone`), so it is created and
removed at every stop rather than left standing; it is removed **after** `vk-ab.py` has run, not
before, because `vk-ab.py`'s own "these captures predate the lever" staleness check only runs while
the lever is still on disk.

| walk | stops at 0 px | non-black px a side, min–max | of |
|---|---|---|---|
| shell, 640×480 (what the shell runs at whatever the game res) | **13 / 13** | 297 477 – 307 200 | 307 200 |
| in game, 640×480 | **13 / 13** | 93 688 – 145 437 | 307 200 |
| in game, 1024×768 | **13 / 13** | 118 232 – 174 781 | 786 432 |
| in game, 1920×1080 | **13 / 13** | 175 576 – 232 125 | 2 073 600 |

**52 of 52**, and **the ink column is half the claim.** On every one of the 52 rows the GL lane's
non-black count and the Vulkan lane's are the **same integer** — not merely both non-zero — so each
0 px is a diff over a frame that had content, and had the same amount of it on both lanes. A row
with 0 px and 0 ink is two blank frames agreeing and is refused, not counted; that is the fourth
guard below, and it is the reason these figures are a re-measurement.

The in-game walks include the four-deep stack `VISUALRT` over `PREFS` over `ARMOPT`
over `ARMCOM1` over `ARMMAIN2`, both pages of the build menu, chat and F4; the shell walk includes
`SELMAP` over `SKIRMISH` and our own injected `VISUALS.GUI` with its 50 gadgets.

**It was 39 until the run that produced these figures, and the extra 13 are the 640×480 walk's
in-game half.** The earlier set took the 640×480 row from a shell-only run, so the resolution the
shell actually renders at had no in-game stops at all; this set walks both halves there. The other
two rows are unchanged — the same stops, the same ink, to the byte — which is also what says the
landing that prompted the re-run moved no pixel.

The in-game ink is a **smaller fraction** of the frame than the shell's because the shell is UI
edge to edge while in game the UI is the panel, the bars and the strings over a world the A/B
blacks — 31–47 % of a 640×480 frame, 15–22 % of a 1024×768 one, 8–11 % of a 1080p one. The
fraction falling as the frame grows is the expected shape: the panel is a fixed pixel size and the
world around it is not. It is the count of pixels **this layer** put down, which is what the
comparison is about.

**A STOP WITH NO USABLE COMPARISON IS NOT A ZERO, and this is the part of the walker that matters
most.** `vk_ab` returns `None`; the stop prints `NO COMPARISON` and the report renders it **NO
COMPARISON** and counts it *out* of the pass tally. Four runs earned four separate guards, every
one of them found by a walk that had already reported a pass:

* **Neither lane wrote.** The first walk ran against an instance a killed run had left part-driven,
  and the game exited a third of the way through. Twelve stops had no pair at all — against a dead
  game. Rendered as `0 px` they would have read as twelve passes, and the walk would have claimed
  17 of 17.
* **A capture caught mid-write.** The first 1080p walk read `tagpu_gui_vk.ppm` at **5 509 120 of
  6 220 800 bytes**, because "both files exist, sleep 0.4 s" is enough at 640×480 and not at 1080p.
  It now polls until each file reports the same size twice running.
* **The settle compared against the wrong number.** That poll then checked the settled size against
  `--res`, i.e. the *game's* resolution — but **the shell runs at 640×480 whatever the game
  resolution is**, so a 1024×768 shell walk waited for 2 359 296 bytes against a real 921 615, never
  settled, and fell through on the 40 s deadline at **all 13 stops**. Every one of them was taken by
  the timeout: by exactly the "both exist, then hope" behaviour the poll had been added to replace.
  The expected size now comes from the **PPM header**, so it is the frame's own resolution.
* **Both captures blank.** `vk-ab.py` prints `differing px 0 of N` *first* and only then decides
  that `diff == 0 and ink_gl == 0` means "BOTH CAPTURES ARE BLANK — that is not a pass" and exits 1.
  The walker read the count and never the exit status, so a stop where the layer composited nothing
  scored **0 px and rendered as a pass**. The A/B blacks the frame and the layer's shader `discard`s
  every fragment it does not own, so two all-black captures agree perfectly and prove nothing. The
  exit status and the `non-black px` line are both read now, and **the ink is a column in the
  report** — a 0 px row is only a pass with a non-zero ink beside it.

* **The walk ran even when there was nothing to walk.** `tacli launch` and, worse, `tacli scenario
  load` both had their exit status captured into `rc` and never looked at. A failed load sent the
  walk into `game_walk` against whatever was on screen — the shell, most likely — where the stops
  would find real content, diff it, and report 0 px with a healthy ink count. Every guard above is
  about telling a hole from a pass at one stop; this was a hole upstream of all of them, and none
  of them could see it. Both calls now pass `check=True` and raise with tacli's own output.
  [FOUND 2026-09-16, reconciling why one walk had recorded no in-game stops at all.]

The middle two were found by a review of the walker *after* it had produced a "39 of 39", which is why
**all three walks were re-run from scratch** under the corrected guards and the numbers below are
the second set. The first set is withdrawn: two of its stops' guards were weaker than the prose
describing them.

This is the failure mode this lane produces over and over — landing 1 stood down on every frame
while every counter read zero, landing 4 measured 0 px on a run that had restored nothing — and the
walk is the one place where a hole and a pass look identical unless the tool refuses to conflate
them. **0 px over content that is not there is not a measurement.**

#### Not covered by the walk

* **IT IS NOT THE `strict` WALK THE GATE'S WORDING ASKS FOR.** It is a lane-against-lane A/B, for
  the reason given at the top of this section. 0 px means the two lanes agree, not that either is
  right; the GL lane's own correctness is G15's, measured elsewhere and inherited here.
* **The blank-pair guard is a refusal, not coverage.** It can tell a stop where nothing was
  composited from a stop where the two lanes agreed on real content. It cannot tell a stop where
  *most* of the content was missing from both lanes: the ink column would be non-zero and the
  diff would be 0. A partial hole common to both lanes still scores as a pass.
* **The size-settle guard never engaged on the shell walk.** The shell runs at 640×480, where the
  captures are 921 615 bytes and the writes have always completed inside one 0.4 s poll; the
  mid-write case was only ever observed at 1080p. So the shell walk's 13 stops exercise the
  *settle-on-equality* path but never the *wait* it exists for, and the guard is evidenced by the
  in-game walks alone.
* **The inventory is the screens, not every state of them.** 13 stops per walk. A build menu page
  the walk does not turn, a dialog it does not open, an animation mid-frame: not covered.
* **One map, one side (ARM), one scenario** — `tascene-parity`.
* **The cursor and the minimap are at their landing-3 levers** (`mmbase`), not swept.
