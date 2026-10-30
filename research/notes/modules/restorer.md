# The restorer

Undithering on by default, the compute restorer and the terrain restore.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

## Undithering ships on, and the UI is restored too

Two facts about Classic++ were false in the shipped configuration until this landing: nothing
undithered, and nothing in the UI could ever have.

### The restorer had a second lever, and it was on no defaults table

`tagpu_classicpp.on` is a play default and logs `classicpp: assets=1 light=1`. What actually fed
the restorer was a separate file, `tagpu_restorevk.on`, tested in exactly two places
(`tagpu_gaf_atlas_restore_vk` for the unit/feature/effects atlases, and `tagpu_terr.c`'s arm beat
for the terrain atlas, which is not a GAF atlas). Nothing armed it, so a player launching the
game got the raw 8-bit palette art of 1997 with a menu row saying `Undithered assets: On`.

**Both sites now ask `tagpu_classicpp_assets()`** — the master arm AND the `assets=` key of
`tagpu_classicpp.cfg`, which is the key the render-options screen's row writes. One question,
one row. The lever is gone as a name; the `restorevk:` log prefix stays, being the restorer's
lane tag rather than the lever.

**"Read at attach, latched per pass" was never true of those sites and is not a constraint.**
Both were already POLLED — the GAF one on each owner's 30-frame arm beat, the terrain one on its
own — so the latch takes on the first beat at which the answer is yes. The latch is a LIFETIME:
it guards one `malloc` whose address a consumer holds raw for the rest of the frame, which is
why it must not be re-taken. Turning the knob OFF is answered where it always was, by the
publishers' `restored` flag and by `tagpu_vk_restore.c`'s `may_draw`, which put `uRestored` back
to 0 and pause the job in place without freeing anything.

Measured at 1024x768 on `pose-inventory`, Two Continents, play defaults, no arm file:

| | whole frame | grass (330,560)-(430,640) |
|---|---|---|
| before | 12 428 colours | 14.21 energy / 193 colours |
| after | 94 532 colours | 7.16 / 4 264 |

which is the same picture the hand-armed lever gave (94 538 / 7.16 / 4 264). The terrain job is
5062 frames in 80 batches, 3760 draws across 137 of 147 frames = **2426 ms of wall clock at 60.6
fps**, 1641 ms of it GPU, against a 12 ms/frame budget. The first two to three seconds of a level
are visibly indexed and then reveal centre-out; the frame rate does not move.

### The UI atlas was the one GAF atlas that armed no list

`tagpu_gui_surf.c` reported `col=0/3 colvalid=0 rgb=0` on every frame of every session, and the
reason was not a switch. `twin_sprite` and `twin_copy` returned 0 unconditionally, `s_colValid`
had no writer, and `tagpu_vk_gui.c` carried the whole consumer — `tw_colour`, the two-attachment
render pass, the four `*2` pipelines, `LAY_FS`'s `uColOn` and `uTwinCol` — as reachable code that
nothing could reach. `TAGPU_R_MAXJOBS` had reserved priority slot 4 for the UI since landing 7.

What was built is the same shape as the feature atlas's: the UI atlas arms
`tagpu_gaf_atlas_restore_vk`, publishes its frame list on the hand-over, and `tagpu_vk_gui.c`'s
`restore_want` paints a `"gui"` job into an RGBA8 image of its own, bound at descriptor 41
(`uAtlasRGB`) on the ops that say they sample it.

**Three things had to be facts rather than hopes, and each was found by running it.**

1. **An op may not claim restored art before there is any.** The consumer refuses such a frame
   WHOLE — correctly: it would write alpha 0 where the producer wrote colour, into a twin that
   keeps it. Deriving "restored" on the producer's side alone therefore thrashes the twin store
   for the two seconds a first restore takes. `tagpu_gui_col_ready` is the back-channel: the
   consumer says whether it holds a painted image, and only then does `s_colValid` rise. Both
   halves are the render thread, so it is a static and not a handshake, and the producer reads it
   one frame old — which can only delay turning colour ON.

2. **The art has to be REDRAWN to gain colour.** A twin takes its restored texels at the moment
   the art is drawn, so anything already on a surface keeps the indices it was painted with. In
   the shell that is invisible (every gadget is redrawn on every flip); in game it is the whole
   sidebar, which the engine draws once per selection change. Measured: with the panel left
   standing, `assets=0` and `assets=1` were **0 differing pixels of 82 944** over
   (0,120)-(128,768); one deselect-and-reselect took the same region to 23 662 colours.
   `g_guiq.colarm` asks the engine for that repaint — one counter, shadowed by
   `tagpu_gui_hook.c`'s `repaint_arm` exactly as `g_guiq.resets` is.

   **It is deliberately not a reseed, and that is a fix rather than a preference.** The first
   build raised `g_guiq.reseed` on the same edge. A reseed resets the twin store and the UI
   atlas, which re-arms the restore list, which clears the consumer's image, which clears the
   very flag that raised it: the loop ran, the layer composited nothing at all, and the harness
   showed a magenta frame.

3. **And once is not enough, for a bounded reason rather than a timing one.** A sprite drawn in
   the same present that put its entry in the atlas takes alpha 0, the restorer not having
   reached that entry yet. So the producer asks for one more repaint each time the restore
   SETTLES having painted something new. It converges because the set of atlas entries a screen
   uses is finite — a repaint that adds no entry adds no frame, and no frame is no settle — and
   it is bounded at 32 per palette generation anyway, with a line in the log if that is reached.

**The palette-validity rule of [gui-renderer](../gui-renderer.html) 3.4 is now built rather than
described.** While the presented palette differs from the one the restore was painted against,
`s_colValid` is 0 and the frame is indexed — dithered art for the duration of a fade, never wrong
art — and once the palette has held still for `PAL_SETTLE` frames the list is restarted as a
REPAINT (`tagpu_gaf_atlas_restore_repalette`, the generation `rlistRepaint` was documented for
and nothing had ever asked for) and `colRearm` invalidates every colour plane the consumer holds.

Measured at 1024x768, `assets=0` → `1`, commander selected:

| region | before | after | pixels differing |
|---|---|---|---|
| sidebar (0,120)-(128,768) | 11.53 / 273 colours | 10.07 / 23 662 | 44 061 of 82 944 |
| HUD patch (20,300)-(110,420) | 17.61 / 115 | 15.18 / 6 453 | |
| minimap | 9.21 / 543 | 8.37 / 3 246 | 3 576 of 15 624 |
| top bar | 10.01 / 89 | 9.98 / 598 | 761 of 28 672 |
| whole frame | 12 468 colours | 115 666 | |

and the shell (MAINMENU, 640x480) goes from 148 to 2 585 colours, 9 191 px of 307 200. The
heartbeat reads `col=5/5 colvalid=1 rearms=1 rgb=41/71` and the frame rate holds 60.0.

**What is not restored, said plainly.** A surface adopted whole from the engine's bytes —
`PK_ASSET`: the shell's backdrop, the in-game panel's ground plate — carries palette indices and
nothing else, so it stays dithered. That is why the shell moves 3 % of its pixels rather than
30 %. Restoring those needs a per-surface restore rather than an atlas one, and is the next
thing to build here.

**`rgb=` in the `gui:` heartbeat was a dead GL texture name** and is now
`rgb=<frames offered>/<atlas entries>`. `colops=` and `prescol=` are new beside it: `colvalid=1
colops=0` is the difference between the restorer being off and the ops that would sample it never
running, which is the distinction this landing spent three builds failing to see.

### 2.102 The restorer as Vulkan compute, its launch self-test and its crash guard (`tagpu_vk_restore.c`, `tagpu_restore_comp.h`, `tagpu_restore_ref.c`, `tagpu_restore_guard.c`) — 2026-09-27

**What it is.** Landing 1 of [the fp32 compute restorer](../compute-restorer.html): the restorer's
backend is compute shaders (FILL, one CONV per layer shape, OUT, MIP) instead of fragment passes,
under the same core, for all six jobs (terrain, features, effects, units, the UI and its
pictures). Nothing it draws moved: every job is within one level of the torch reference on at
most 0.0019 % of bytes (the table is in the plan note). The fragment backend, its shader header,
the `fp16`, `nk=` and `tiny` tokens of `tagpu_restoreglsl.on`, and the browser lab's
`restore=glsl` lane are deleted.

**No engine address is patched or read.** What it does to the process: an unhandled-exception
filter, installed at the restorer's first bring-up with `SetUnhandledExceptionFilter` ahead of
TA's own `0x4DA2A0` (MEASURED: the previous filter it logs is `004DA2A0`). TA's filter calls no
earlier filter, so ours has to run first; it acts only when the render thread faults inside a
backend call that builds or dispatches — never in the teardown — and hands every other exception
to TA's. After a crash it starts TotalA.exe again, and the new process waits at
`DLL_PROCESS_ATTACH` for the old one to end, because TotalA.exe exits silently while another copy
holds its single-instance semaphore (`0x49E885`). The engine side is in the
[engine map](../exe-reverse-engineering.html), *TA's crash filter* and *TA's single-instance test*.
The record is keyed `vendor:device:driver:build`, so a new driver or a new DLL tries again. The
crash marker is read at the first bring-up only, becomes the record only when it names this
device and build, and stays on disk until the disk holds the record as the process last made it
(its key, or nothing after a retry). The retry is carried out on the render thread in the render
options' per-frame hook, under either backend.

**The seam's part** (`tagpu_vk.c`). Every Vulkan failure that ends the lane (`-2`) notes whether
the device reported itself lost, or a fence or an acquire only timed out; a timeout asks
`vkDeviceWaitIdle` and counts as lost only when that answers `VK_ERROR_DEVICE_LOST`. A loss asks
the guard, which blames the restorer only when a frame carrying restorer work is still
unfinished: the backend sets a bit for the frame slot whenever its step records anything, and the
seam clears it after that slot's fence wait, and for every slot once the device is proven idle or
destroyed. A blamed loss records the device off and relaunches the game; any other still takes
the Vulkan renderer down as before.

**The consumers** (`tagpu_vk_terr.c`, `_feat.c`, `_fx.c`, `_unit.c`, `_gui.c`). Each one's "the
restorer refused" latch now holds for one restorer epoch; the render options' retry moves the
epoch, so a consumer that gave up asks again. Every destination twin carries STORAGE usage,
which OUT writes through.

**What it costs** (the same instance and scenario, `feat-forest` on Two Continents, 1024 × 768,
vsync off, RTX 4070): the terrain in 996 ms of wall time and 389 ms of GPU, against the fragment
backend's 3,578 and 1,627 ms. The self-test adds two probe jobs at the first bring-up, and a CPU
reference of 313–339 ms on a worker thread while the game starts.

**On the Windows test setup's AMD card** (MEASURED, the plan note has the table): the launch
self-test failed on its first run there, rightly. FILL wrapped with a float floor division, and
that driver divides through the reciprocal, so a 6×8 wrapping frame read outside itself (up to 10
levels off) while an 8×8 one was exact. FILL's modulo is integer arithmetic now. With that, the
self-test passes and every job that ran is within one level on at most 0.0022 % of bytes; the
terrain takes 3.8 s of wall time and 2.55 s of GPU, against the fragment backend's 10.6 and 7.0
(§2.94). §2.95's barrier fix belonged to the fragment backend's render passes; compute has none.
The crash path ran there too: the filter ahead of `004DA2A0`, one relaunch, the record, the notice.

**Not covered.** The effects job on the AMD card (the scene drew no effects). The timeout rule
(no lever makes a fence time out on a live device). The residuals the plan note lists: a relaunch
that can stop inside a crash holding the heap lock (the marker is on disk first); a store that
cannot be written, which leaves the marker standing as the record and the notice showing once per
launch until the row's retry supersedes it; a device that hangs for good without reporting the
loss, which is not blamed, because nothing tells it from a long stall; and a restorer frame over
the fence's one-second wait that still finishes, which takes the lane down unblamed, as any
timeout does.

### 2.103 The terrain restored by neighbourhood, on tiny (`tagpu_terr.c`, `tagpu_vk_terr.c`, `tagpu_restore_core.c`, `tagpu_restore_comp.h`, `tagpu_vk_restore.c`, `tagpu_vk.c`) — 2026-09-27

**What it is.** Landing 2 of [the fp32 compute restorer](../compute-restorer.html): a tile restored
alone sees zeros past its edge where the map has the next tile, so its edge was not what a restore
of the whole map gives there. Each cell is now restored with its eight neighbours around it, once
per distinct neighbourhood (a *key*), into a neighbourhood atlas the terrain samples through a
per-cell key; the terrain's job runs the tiny model (D3), every other job full. Two Continents:
43,658 keys over 134,400 cells, a 7106 × 7106 atlas of 192 MB. Every cell of the map, its ring
included, is within one level of a whole-map torch restore on 0.0021 % of bytes.

**What it reads of the engine** is what the per-tile order already read, where it reads it: the
tile map `main+0x1428B` and the map's dimensions `main+0x14233`/`+0x14237`, once per map when the
restore request is built, under the teardown fence (`tagpu_terr.c` is `fenced` on
`thread-split.allow`). `nb_build` copies the map once and bounds every tile id in the copy by the
atlas's tile count before it makes a key. **Nothing is patched.**

**The hand-over** (`tagpu_terr.h`). The instance record is eight shorts (`TAGPU_TERR_ICOMP`): the
grid cell, the tile's cell in the base atlas with the mirror flags, and the key's cell. The
hand-over gains the key frames in atlas order, the centre-out order over them and the atlas's
size; `nbOn` says this frame's records carry keys. The terrain's vertex shader samples the
restored colour at the key's cell when `uNbhd` is set, and the base atlas's tile otherwise.

**The feed.** One terrain job, fed each frame (`nb_feed`): the keys of the cells on screen not yet
queued go in, and while any key on screen is not painted — however it was queued, each key keeps
its place in the FIFO queue — the job runs at prio 0 with the whole budget; otherwise the rest of
the map follows centre-out, one batch queued behind the one in flight, at prio 6 with a 2 ms GPU
cap (`tagpu_rcore_job_budget`). A resize that retires an image the job uses frees the job first:
the retire ends the image's use only if nothing names it afterwards, which the terrain pass
guarantees for its own descriptor sets and a live job would not. A new job's first barrier on its
destination (`dst_ready`) now waits for every earlier user of the image, the transition out of
`UNDEFINED` included, where it chained to nothing before. Two jobs painting one image, as the plan had it, would each clear and
move the destination on their own schedule. Two Continents at 1024 × 768 on the RTX 4070: the first
screen's 479 keys in 114 ms, the whole map in 19.3–26.3 s, Lava Alley's in 3.2 s.

**Whether it fits** (D10): the atlas against half of what the driver says is free —
`VK_EXT_memory_budget` through `VK_KHR_get_physical_device_properties2`, which the instance now
enables when the loader offers it — or a quarter of the largest device-local heap. **A seam fix
rode with it:** the device-extension walk in `tagpu_vk.c` stopped as soon as its two-entry array
was full, which `VK_KHR_maintenance1` fills, so no extension listed after it was ever seen; the
bound now guards the append. Where the atlas does not fit, the device refuses it, the keys need an
image wider than the device allows or than the pass carries (16384), the map names a tile past the
atlas, or a refusal of the restorer stands (asked before the atlas is sized, so a device recorded
off allocates none), the terrain restores per tile as in §2.102, on tiny.
`tagpu_restorefault.on`'s `nbfit` drives the first, `nballoc` the second on the first map after
the pass comes up.

**The self-test** gains a neighbourhood probe on tiny: nine frames over a 3 × 3 map, checked
against one reflect-padded restore of the whole map on the CPU (22,696 bytes within 0 levels).

**On the Windows test setup's AMD card**, on the landing's last build, the self-test passes, the
budget query answers (2,105 MB free of 2,816), the whole map paints in 30.4 s at about 59 fps, and
every cell is within one level of the whole-map restore on 0.0007 % of bytes (the plan note has
the table).

**Not covered.** How long a key scrolled onto the screen waits was not measured (by construction it
is at prio 0 behind at most two trickle batches and the screen's own keys). At the
mirrored edge (§2.90) the half-texel at the fold now samples the key's ring, the network's output
for the reflected map, where it sampled a copy of the edge; the fold stays continuous. The tile-grid
lines are the art's and stay (the plan's TODO).
