# The fp32 compute restorer

**Landing 1 is built** (2026-09-27): the restorer's backend is Vulkan compute, with a self-test at
every launch and a per-driver record when it fails. It is verified on the reference setup and on
the Windows test setup's AMD card. **Landing 2 is built** (2026-09-27): the terrain restored with
its eight neighbours, so a tile's edge is what a restore of the whole map gives there, on the tiny
model, with the per-tile atlas where the neighbourhood atlas does not fit. fp16 (cooperative
matrices) is deferred.

The restorer is the learned unditherer Classic++ runs on the game's art ([Classic and Classic++
renderers](renderers.html), [Undithering screenshots](undither.html)). The API-free core
(`tagpu_restore_core.c`) owns the job queues, the batches, a GPU-time budget per frame and the pass
sequence fill → one conv per layer → out. Landing 1 replaced the fragment-shader backend under it
with compute (`tagpu_vk_restore.c`); landing 2 uses the speed for terrain restored across tile
seams.

## What the benchmark measured

An out-of-game Vulkan benchmark (not in the repository), 2026-09-26, on the reference setup's
RTX 4070: the full model over a 1280×960 region of King of the Hill, bytes compared with the
torch fp32 reference.

| path | ns per image pixel |
|---|---|
| fp32 compute, whole image | 49.4 |
| fp32 compute, 512-px chunks with a 12-px apron | 58.5 |
| cuDNN fp32 (torch, best algorithm per layer) | 63.6 |
| the fragment backend the game runs today | ~300 |

Against the torch reference, the fp32 kernel differs on 0.0011 % of bytes, by at most one level
(the same region, rerun 2026-09-27).

- The fp32 kernel stays inside Vulkan 1.0's minimums: 12 KB of shared memory, 128 invocations,
  storage buffers under 128 MB. It needs no extension, so it is the path every Vulkan device
  gets.
- A 12-px apron makes chunking exact: identical bytes at chunk sizes 37, 64, 100 and 200.

## Landing 1, as built

### The backend

`tagpu_vk_restore.c` is a Vulkan compute backend under the unchanged core
(`tagpu_restore_core.c`); the fragment backend, its shader header and its settings are gone. The
shaders are `tagpu_restore_comp.h`, compiled by `tools/spirv-gen.py`:

| shader | what it does |
|---|---|
| FILL | the model's input, one vec4 per texel; a keyed texel takes the mean of the opaque texels on the nearest Chebyshev ring within the model's depth (wrapped for a wrapping frame, clipped otherwise); 0 outside every slot's rect |
| CONV | one 3×3 layer; a workgroup is 16 × TH texels and all output channels, each invocation 8 texels × 8 channels, the input staged 4 channels at a time with its halo. At most 12 KB of shared memory and 128 invocations. One variant per layer shape, read out of the shipped weight files at build time |
| OUT | `in − net`, written as `(k + 0.25) / 255` into the consumer's twin (a storage image) with the frame's border and slack ring as a copy of the edge; `(0, 0, 0, 0)` at a keyed texel |
| MIP | the unit chain's levels, the exact integer `(sum + 1) / 4` of each 2×2 box |

- **The grid.** A batch is a square of up to 8 × 8 slots, pitch S + 1, so every slot ends in a zero
  column and row and a 3×3 tap never reaches a neighbour's rect: that is the padding rule by
  construction. The activations are two storage buffers (the ping-pong), `[gh][gw][channels]` fp32.
- **Bands.** A conv layer is dispatched in bands of 64 rows (`TAGPU_R_BAND`), so the core's
  budget (12 ms of GPU time per frame by default) can stop between them on a slow device.
- **The slot table** goes into the stream with `vkCmdUpdateBuffer` at each batch's FILL.
- **Ordering** is pipeline barriers only; there are no render passes. One barrier after each stage
  (FILL, a layer's last band, OUT) from compute writes to every later reader, and one at the head
  of each slice whose first scope is every earlier submission on the queue, which is what lets
  the activations and the table be shared across slices instead of copied per frame slot.
- **The weights** are the same `<model>.w32.bin` (`unditherer/weights.py`), repacked once at
  bring-up into the conv kernel's layout and uploaded to one storage buffer.
- **Settings.** `tagpu_restoreglsl.on` takes `log` and `budget=<ms>` (0.5 to 100) and nothing
  else (D11).

### The launch self-test (D2, D13)

The first bring-up on a device closes the core's gate, so consumers' jobs queue but nothing of
theirs is dispatched. Three probe jobs (priority −1) then run through the same pipelines: a base
atlas with a keyed frame (a disc, a strip and lone keyed texels, with right-hand slack) and a
wrapping frame, restored into a twin with a two-level mip chain; an R8 atlas with a keyed
frame (bottom slack) and a wrapping frame; and, since landing 2, a map of neighbourhood frames on
tiny (*The self-test* under landing 2). Nothing in the probe comes from the game. While the
GPU runs it, a worker thread computes the same frames on the CPU from the loaded weights
(`tagpu_restore_ref.c`). The readback passes when every colour byte is within one level of the
CPU's and every alpha byte equal, the border and slack rings included, and the mip levels are the
exact box average of the level above; a pass opens the gate.

- **Wrong bytes**: the device and driver are recorded off, every job fails, the notice shows.
- **Could not run** (the probe could not be built, a probe job failed): the restorer is off with no
  record until the restorer next comes down and up — a shell↔game switch, or a swapchain rebuilt
  at a new size, image count or format, which takes the passes down — and that tests again. A
  swapchain rebuilt at the same size, image count and format (a vsync toggle can be one) keeps
  the passes and does not.
- A device that passed is not tested again in the same process.

On the reference setup the CPU reference takes 313–339 ms and the readback matches it exactly
(11,032 bytes, 0 levels).

### The guard (D2, D12, D14) — `tagpu_restore_guard.c`

- **A crash.** An unhandled-exception filter, installed at the restorer's first bring-up ahead of
  TA's own (`0x4DA2A0`, which writes the crash report and calls no earlier filter). It blames the
  restorer only while the render thread is inside a backend call that builds or dispatches
  (bring-up, a job's creation, a chain, a step); everything else goes on to TA's filter. The
  teardown (a job's free, the lane going down) is not guarded: a fault there is the driver
  freeing objects, and at exit it would relaunch a game the player has just quit. Unhandled only,
  never vectored: drivers raise and catch first-chance exceptions of their own.
- **A lost device.** The seam blames the restorer when the device reports itself lost while a
  frame that carried restorer work is unfinished (a per-slot mask, set when a step records
  anything, cleared after that slot's fence wait, and cleared for every slot once the device is
  proven idle or destroyed). A fence wait or an acquire that times out is not a loss by itself —
  a presentation stall does that too — so the seam asks `vkDeviceWaitIdle` and blames only when
  it answers `VK_ERROR_DEVICE_LOST`. A device that hangs and never reports the loss is not
  blamed: nothing tells that hang from a long stall, and the wait does not return (see *Not
  closed*).
- **Relaunch.** For a crash or a lost device, the process writes `tagpu_restore_crashed.txt`
  from buffers built at install (nothing of ours on that path allocates), starts `TotalA.exe`
  again with its own command line and working directory plus
  `TAGPU_RESTORE_RELAUNCHED=<pid>:<creation time>`, and ends. The new process waits at
  `DLL_PROCESS_ATTACH` until the one that crashed has ended — the pid and its creation time, so a
  reused pid is not waited on — because TotalA.exe exits silently while another copy holds its
  single-instance semaphore ([exe map](exe-reverse-engineering.html) `0x49E885`). There is no
  relaunch when the marker could not be written, and a relaunched process never relaunches.
  Where a lost device cannot relaunch (a relaunched process, or `CreateProcessA` failing), the
  process records the device off itself and shows the notice, since it carries on: the lane's
  retry then comes up without the restorer.
- **The record** is one key, `vendor:device:driver:build` — the ids in hex, the build the DLL's
  commit (`-dirty` from an unclean tree): `restoreoff=` in `impure.cfg`, or
  `tagpu_restore_off.txt` under `tagpu_defaults.off`. The build is in it so that a DLL which
  fixes a fault does not inherit the record of the one that had it; a record naming another
  device, driver or build is dropped and the restorer tried again. The next process reads the
  marker at its first bring-up only: one that names this device and build becomes the record and
  shows the notice, once; one that names another key is dropped, as its record would be. The
  marker is deleted once the disk holds the record as this process last made it — that key, or
  nothing after a retry or a drop (the store's flush): until then the marker is the record, and a
  launch that dies first reads it again.
- **The notice** is one message box on its own thread saying what happened and that it is tried
  again by itself after a driver or game update, or from the render options.
- **The render options.** *Undithered assets* has a third stage, **"Off (driver)"**, shown while
  the record names this device. D12 asked for "Off (this driver)"; that text overran the row's
  three stage lights, so it is shortened. Picking On while the device is recorded off — from
  "Off (driver)" or from Off — asks for the record to be cleared; the render thread's next tick
  clears it and then moves an epoch. The tick runs in the render options' per-frame hook under
  either backend, before the store's flush, so a retry lands even when the Vulkan lane has gone
  down for good. The backend's refusal and every consumer's "the restorer
  refused" latch hold only for the epoch of their attempt, so all of them ask again; another
  failure turns it off again. Under `tagpu_defaults.off` the row is greyed like the rest of the
  store's rows, and the file record is cleared by deleting it.
- **The fault lever** `tagpu_restorefault.on` drives each path: `probe` spoils one byte of the
  self-test's readback, `crash` faults inside FILL, and `lost` has the seam report a device loss
  while restorer work is in flight.

### Verified on the reference setup

**D11's bar**, by `tools/restore-dumpcheck.py` on the dumps of `tagpu_restoredump.on`
(`feat-forest`, Two Continents). It restores every dumped frame again with torch fp32 (TF32 off)
from the source dumped beside it, with the DLL's key stand-in, and holds the twin to it:

| job | frames | bytes that differ | max | also checked |
|---|---|---|---|---|
| terrain | 5,062 | 152 of 15,550,464 (0.0010 %) | 1 | ring 0 of 2,672,736 |
| features | 22 | 0 of 61,911 | 0 | 19,553 keyed texels (0,0,0,0) |
| effects | 10 | 0 of 4,242 | 0 | 1,159 keyed texels |
| units | 57 | 1 of 236,544 | 1 | mip levels 1 and 2 exact |
| UI | 3 | 6 of 322,032 (0.0019 %) | 1 | 416 keyed texels |
| UI pictures | 9 | 17 of 2,906,289 | 1 | — |

**The guard**, one launch per path: the self-test passing; `probe` failing it (the notice, the
row reading "Off (driver)"); the record holding on the next launch; the row's retry, both from
"Off (driver)" and through Off back to On; `crash` and `lost` each relaunching once, the new
process logging that it waited for the old one (or, on another run, that the old one had already
ended), then the record, the notice and the marker's deletion once the record was on disk; the
file record under `tagpu_defaults.off`; a record without a build (`10de:2786:94d50000`) dropped
as a new build, and the self-test passing after it; a marker from another build dropped with no
notice. With the store's writes failing (a directory where its temporary file goes): the marker
stays pending, a second bring-up (into a skirmish) does not convert it again, and the row's retry
supersedes it — the marker deleted, the self-test passing. In a relaunched process, a retry
followed by `lost` again: no second relaunch, the device recorded off in the process, the marker
deleted once the record was written, and the lane's retry up with the restorer off. The timeout
rule has no fault lever and was not exercised.

**The cost**, the same instance and scenario (`feat-forest`, 1024 × 768, vsync off, a private
display, the RTX 4070), terrain only:

| backend | wall | GPU | fps while restoring |
|---|---|---|---|
| fragment (main before this landing) | 3,578 ms | 1,627 ms | 39.4 |
| compute | 996 ms | 389 ms | 42.2 |

### Verified on the Windows test setup

The AMD Radeon R9 200-series card on its Windows driver (`1002:6798:0080005b`), 1920 × 1080,
`feat-forest`, through a `tacli` remote instance.

- **The self-test caught a real fault on its first run there.** The palette probe's 6×8 wrapping
  frame was off by up to 10 levels over its whole area, while the 8×8 one was exact: FILL wrapped
  with a float floor division, and this driver divides through the reciprocal, so `floor(6/6)`
  could land on 0 and the tap read outside the frame. The restorer turned itself off, recorded
  the driver and raised the notice (the log's "the player is told"), as designed. FILL's modulo is integer arithmetic now, and the
  reference setup's bytes did not move.

**With the fix**, the self-test passes (11,032 bytes within 0 levels; the CPU reference in
876 ms), and the dumps meet D11's bar:

| job | frames | bytes that differ | max |
|---|---|---|---|
| terrain | 5,062 | 150 of 15,550,464 (0.0010 %) | 1, ring exact |
| features | 24 | 0 of 65,088 | 0, 21,694 keyed texels (0,0,0,0) |
| units | 57 | 1 of 236,544 | 1, mip levels 1 and 2 exact |
| UI | 3 | 7 of 322,032 (0.0022 %) | 1 |
| UI pictures | 10 | 21 of 3,872,886 | 1 |

The effects pass drew nothing in that scene, so its job had nothing to restore; it runs FILL and
OUT on a keyed base atlas exactly as the features' job does.

- **The terrain** restored in 3,846 ms of wall time and 2,551 ms of GPU at 56.4 fps; the fragment
  backend took 10.6 s and 7.0 s on the same card ([status](gpu-status.html) §2.94).
- **The guard on Windows**: the filter installed ahead of `004DA2A0`; `crash` relaunched the game
  from its own process, and the relaunch logged that it will not relaunch again, turned the marker
  into the record (`crash c0000005`) and raised the notice. No `ErrorLog.txt` was written. With
  the key carrying the build (`1002:6798:0080005b:8e896d4`) the self-test passes (CPU reference
  716 ms), and on `crash` the relaunch recorded the device and deleted the marker once the record
  was on disk. It logged the old process as already gone, from a build that did not yet tell an
  ended process from one it could not open; the reference setup has logged both outcomes since.

### Not closed by landing 1

- **The crash filter's relaunch** calls `CreateFileA` and `CreateProcessA`, which are the
  system's and may take the process heap's lock. A crash that holds it can stop there; the marker
  is written first, so the next launch by hand still has the record.
- **A settings store that cannot be written** never gets the record, so the marker stays on disk
  as the record: the restorer stays off for that device and the notice shows once at every
  launch, until the store can be written or the row's retry supersedes the marker.
- **A hang that never reports itself lost** is not blamed. The seam asks the device after a
  timeout, and that wait does not return from a device that hangs for good, so no record is
  written; the next launch runs the restorer again. A driver whose watchdog resets a hung GPU
  reports the loss, and that is blamed. A provisional marker written before the wait would catch
  the hang, and would also record a working driver off whenever the player quits during a long
  presentation stall; it is not built.
- **A restorer frame slower than the fence's one-second wait that still finishes** is not blamed
  either, and the lane goes down as for any timeout. A slice is two dispatches before its cost is
  measured and the 12 ms budget after (six dispatches where the device cannot time one), so
  reaching a second is a device far below anything the self-test has run on.
- **The timeout rule is not exercised**: no lever makes a fence time out on a live device.

## Landing 2, as built

The terrain seam fix (D5–D10) and tiny for terrain (D3), built 2026-09-27 on the branch that
carries landing 1.

### A model per job (D3)

The core loads `full.w32.bin` and `tiny.w32.bin`, and a job names its model (`TAGPU_RM_FULL`,
`TAGPU_RM_TINY`, `tagpu_restoreglsl.h`): the terrain's job is tiny, every other job full, and a job
whose model is not loaded runs full. The backend repacks both into one weights buffer, keeps a
conv pipeline per layer of each model, and sizes the activations for the wider. Both files ship in
the release folder (`tagpu/release/package.sh`).

### The neighbourhood frame

A neighbourhood frame (`TAGPU_RNBFRAME`, `tagpu_restoreglsl.h`) restores one cell of the map with
its eight neighbours around it. `f` is the centre — (ax, ay) its tile in the base atlas, (dx, dy)
its cell in the neighbourhood atlas, `border` 1 — and `nbo[8]` holds each neighbour's tile origin in
the base atlas (`x | y << 16`, in the order NW N NE W E SW S SE). A neighbour past a side of the map
is the cell inside reflected across it, with that side's `edge` bit (L 1, R 2, T 4, B 8) telling the
shader to flip its texels, so texel −1 of the map is texel 0 — the mirror rule the whole-map
reference pads with. `tagpu_rcore_nb_frame` builds one from a map cell; the terrain's keys and the
self-test's map are both built by it.

- **The window** is the tile plus `a = depth + border` texels of its neighbours on every side
  (tiny: 7, a 46-texel slot). The depth makes the centre exact and the border makes the cell's
  one-texel ring exact too, so the ring holds the network's output over the real neighbours
  instead of a copy of the edge. `tagpu_rcore_job_add_nbhd` refuses a frame whose window would
  reach past one neighbour (`a` larger than the tile) or that carries slack: that bound is what
  the shaders' addressing rests on.
- **FILL and OUT** read the input through one function, `nbAt` (`tagpu_restore_comp.h`): a window
  texel is in the centre or in exactly one neighbour, flipped across an edge side. OUT needs it
  too, because the network predicts a residual, and the input at a ring texel is the neighbour's.
- **Nothing is uploaded per key.** The window reads the RGBA base atlas the terrain already has.

### The keys and the atlas (D5)

`nb_build` (`tagpu_terr.c`) runs where the restore request is built, on the same fenced read of
the tile map as the per-tile order (`TILE_MAP` `main+0x1428B`, the dimensions `main+0x14233`/
`+0x14237`): the map is copied once, every tile id in the copy is bounded by the atlas's tile count
before any key is made, and each cell's frame, less its destination, is looked up in a hash of the
keys so far. **A key is one distinct frame** — the centre's origin, the eight neighbours' and the
edge bits — and the per-cell key index is 32-bit on the CPU. Each key's rank is the Chebyshev
distance from the screen's centre cell to the nearest cell that uses it, and a counting sort gives
the centre-out order.

- **The atlas** is one RGBA8 image, near square, one 34-texel cell per key; `s_maxTex` (the
  device's `maxImageDimension2D`) bounds its side.
- **The instance record** is eight shorts: the grid cell and the tile's cell in the base atlas with
  the mirror flags, then the key's cell. The vertex shader takes the restored colour's UV from the
  key's cell when `uNbhd` is set, and from the tile's otherwise; a key not painted yet has alpha 0
  and the cell draws dithered (D7).

| map | cells | keys | atlas | memory |
|---|---|---|---|---|
| Two Continents | 336 × 400 | 43,658 | 7106 × 7106 | 192 MB |
| Lava Alley | 295 × 297 | 12,149 | 3774 × 3740 | 53 MB |

D5's estimate for Two Continents was 193 MB.

### Whether it fits (D10)

`tagpu_vk_restore_fits`: the atlas's bytes against half of what the driver says is free —
`VK_EXT_memory_budget`'s budget less its usage for the largest device-local heap
(`tagpu_vk_mem_free`), read through `VK_KHR_get_physical_device_properties2`, which the 1.0
instance enables only when the loader offers it — or, where the query is not offered, a quarter of
that heap. The log says which rule answered. On the reference setup: 192 MB against half of the
10,203 MB the driver says is free, of 12,282 MB.

**The per-tile fallback** is landing 1's atlas and frames, on tiny. It is taken when the fit says
no, when the device refuses the image, when the keys need an image wider than the device allows
or than the terrain pass carries (16384), when the map names a tile past the atlas, and while a
refusal of the restorer stands: the choice asks the restorer first (a device recorded off
answers then), so no neighbourhood atlas is allocated for a job that will not run. The choice is
made once per request and restorer epoch, so the render options' retry chooses again. A job is of
one layout (`s_rjNb`): when the choice moves under a standing request, the job goes and the
picture stops being sampled, as for a new map — the two layouts can have one size, so a resize
would not catch it.
The fault lever drives the first two: `tagpu_restorefault.on` holding `nbfit`, or `nballoc` on the
first map after the terrain pass comes up, where no restored atlas exists yet.

**The job goes before its images.** A resize retires the image it replaces once every frame slot
has been through its fence, which covers every command buffer submitted until then. That is the
image's last use only if nothing names it after the retire begins: the terrain pass guarantees it
for its own descriptor sets, but the restorer records its dispatches after every pass's prepare,
and a job left alive would name the old image on the next frame. So a resize that will retire an
image the job reads or writes frees the job first (`rjob_release_for`); a new job starts on the new
image. A frame whose records carry no keys does not size the neighbourhood atlas at all.

**A new job's destination waits for the image's earlier users.** The job's first barrier on its
destination (`dst_ready`) has every stage the image has been used in as its first scope — the
terrain's sampling in frames still in flight, an earlier job's writes, a dump's copy — including
the transition out of `UNDEFINED`, which discards the contents but is still a write.

### The feed (D8)

**One job, fed, not two.** The design had an on-screen job and a trickle job painting the same
image; two jobs over one destination would each clear it and move its layout on their own
schedule, which nothing orders. So the neighbourhood job is fed (`nb_feed`, `tagpu_vk_terr.c`):

- **Each frame**, the keys of the cells in the frame's records — the cells on screen — that are not
  queued yet go in, and while any key on screen is not painted the job runs at prio 0 with the
  whole budget.
- **Otherwise** the rest of the map follows in centre-out order, topped up to one batch (64 keys)
  waiting behind the one in flight, at prio 6 — after every other job — with a cap of 2 ms of GPU
  a slice.
- **"Not painted" is exact**: every neighbourhood is one size class, so the queue is FIFO, and a
  key is painted exactly when the job has painted more frames than its place in the queue, which
  each key keeps. That holds for a key the trickle queued as much as for one queued on screen, so
  a key scrolled onto the screen puts the job at prio 0 however it was queued, and waits behind at
  most the keys queued before it: the two trickle batches and the screen's own.
- **The core** gains a budget per job (`tagpu_rcore_job_budget`): a capped job stops at its cap,
  counted across the slice, and is passed over for the rest of it, which the other jobs keep —
  except that its batch in flight runs on at the full budget while another job waits behind it;
  with no GPU timer the cap is two dispatches a slice. `TAGPU_R_MAXJOBS` is 9:
  the six consumers and three probes.
- **The dump waits for the feed** (`tagpu_vk_restore_job_feeding`): a queue that drains between
  feeds is not a finished picture.

On Two Continents at 1024 × 768 on the RTX 4070: the first screen's 479 keys painted 114 ms after
they were queued, in three slices; the whole map in 19.3–26.3 s (three launches), at the cap. The
same 43,658 keys handed to one job at the full budget take 1,131 ms of wall time and 414 ms of GPU
at 38.9 fps. Lava Alley's whole map: 3.2 s. The per-tile fallback, on tiny: 5,062 frames in
226–258 ms.

### The self-test

The launch self-test gains a third probe job, `probe-nbhd`, on tiny: a 3 × 3 map of 16-texel
tiles scattered through the probe's 64 × 64 base atlas, every cell of it a neighbourhood frame.
Its CPU reference is one restore of the whole map, reflect-padded by `depth + border` — the rule the
frames stand for, not their addressing — so a frame that reads the wrong neighbour or flips the
wrong way fails it. The self-test passes on the reference setup with 22,696 bytes within 0 levels,
the CPU reference in 344–385 ms. The same check with the edge bits ignored in the shader fails at
cell 0.

### The bars

`tools/restore-dumpcheck.py` checks each job with the model its dump names (`# model` in the
`.idx`). A neighbourhood frame is restored over its window, and its ring is held to the same bar
as the tile. `--whole-map` rebuilds the map from `tagpu_restore_terr_vk.map` (every cell's tile
and key, written under `tagpu_restoredump.on`) and the dumped base atlas, restores it in one piece
(chunked with an apron of the model's depth, which is exact), and holds **every cell**, its ring
included, to its key's painted cell. Every check passes: at most one level, on under 0.01 % of
bytes.

| map | check | what | bytes that differ |
|---|---|---|---|
| Two Continents | per window | 43,658 keys | 2,585 of 151,405,944 (0.0017 %) |
| Two Continents | whole map | 134,400 cells | 9,769 of 466,099,200 (0.0021 %) |
| Lava Alley (a second map in the same launch) | per window | 12,149 keys | 309 of 42,132,732 (0.0007 %) |
| Lava Alley | whole map | 87,615 cells | 1,949 of 303,848,820 (0.0006 %) |
| Two Continents, per tile (`nbfit`) | per frame | 5,062 frames | 244 of 15,550,464 (0.0016 %), the ring exact |

`nballoc` took the same per-tile path, and its frame on screen is identical to `nbfit`'s. The
other jobs at the same build: features and effects 0 bytes, units 1, the UI 6, its pictures
17, each at most one level.

**On the Windows test setup's AMD card** (`1002:6798:0080005b`, 1920 × 1080, `feat-forest` on Two
Continents, a `tacli` remote instance), on the landing's last build with main merged in: the
self-test passes with the neighbourhood probe (22,696 bytes within 0 levels, in 902 ms); the
memory-budget query answers there (192 MB against half of the 2,105 MB the driver says is free, of
2,816 MB); the whole map is painted in 30.4 s, the restorer working in 1,784 of the 1,801 frames
the game drew meanwhile (59.2 fps). The terrain's dump is byte-identical to the run before the
review's fixes. The dumps:

| check | what | bytes that differ |
|---|---|---|
| per window | 43,658 keys | 1,479 of 151,405,944 (0.0010 %) |
| whole map | 134,400 cells | 3,324 of 466,099,200 (0.0007 %) |

and features, effects, units and the UI as on the reference setup (0, 0, 1 and 7 bytes), each at
most one level. Defender, fully armed there, raised no detection.

### Not closed by landing 2

- **The trickle is seconds, not frames.** Two Continents' whole map takes about 20 s at 2 ms of GPU
  a frame on the reference setup and 31 s on the AMD card; a cell whose key is not painted yet
  draws dithered (D7). How long a key scrolled onto the screen waits was not measured; by
  construction it is at prio 0 behind at most the two trickle batches and the screen's own keys.
- **At the mirrored map edge** (the edge setting of [GPU status](gpu-status.html) §2.90) the
  half-texel at the fold samples the key's ring, which is now the network's output for the
  reflected map rather than a copy of the edge texel. Both sides of the fold blend the same pair,
  so the fold stays continuous; how far that half-texel moved from landing 1's was not measured.
- **The memory is the design's**: Two Continents' neighbourhood atlas is 192 MB against the
  per-tile atlas's 23 MB, and it replaces it rather than sitting beside it.
- **The tile-grid lines** (D6, the TODO below) are the art's and stay.

## Decisions

| # | decision |
|---|---|
| D1 | **Two landings.** Landing 1 swaps the backend: fp32 compute for all six jobs (terrain, features, effects, units, UI, pictures) under the existing core, the fragment backend deleted. It is tested on Windows too, sharing the Windows test machine with the mod-compatibility regression work. Landing 2 is the terrain seam fix. |
| D2 | **A self-test at every launch** runs the same pipelines on a synthetic probe (nothing from the original game) and checks the result against D13's reference. A crash attributed to the restorer records `off` for that device and driver, and the game relaunches at once with a notice. A new driver version, or a new build of the DLL, gets a fresh try. |
| D3 | **tiny for terrain, full for everything else** (units, features, effects, UI). Terrain is the volume; tiny (6 layers × 24 channels, 22k MACs per pixel) costs about a tenth of full (12 × 64, 372k). It was gated on a wider terrain A/B (D15); **the owner judged the sheet on 2026-09-27: tiny wins** — the numbers are under the table. The switch goes in with landing 2: landing 1 changes no picture. |
| D4 | **The browser lab's `restore=glsl` lane is dropped.** The lab keeps `restore=pack`. |
| D5 | **A neighbourhood atlas for terrain:** one restored copy per distinct tile graphic *plus its 8 neighbours*, drawn through a per-cell index. King of the Hill: 2,978 graphics, 69,958 spots, 26,827 keys, 118 MB. Median map 35 MB, Two Continents 193 MB, Seven Islands (the largest) 303 MB, against 14–51 MB for today's per-graphic atlas. A map whose atlas does not fit the GPU's memory budget keeps today's atlas, seams included. |
| D6 | **The tile-grid lines are left for now** — see the TODO below. |
| D7 | **No quick per-tile pass first.** Terrain restores straight into the neighbourhood atlas, spots on screen first; a spot not restored yet draws dithered, as it does during today's reveal. |
| D8 | **The GPU budget during play:** spots on screen get the full budget (12 ms per frame); the rest of the map restores at a fixed trickle of about 2 ms. Spots scrolled onto the screen move to the front. |
| D9 | **No disk cache.** The terrain is restored every time a map is played. |
| D10 | **Whether the atlas fits** is decided before allocating. Its size is exact once the map loads (keys × cell size). The driver's memory-budget query gives the memory still free for the game; a driver without that query counts a quarter of the GPU's device-local memory as free. The atlas fits when it takes at most half of that free memory. An allocation the driver still refuses falls back as well. |
| D11 | **Settings and the bar for landing 1.** The `fp16`, `nk=` and `tiny` settings are removed; `budget=` and `log` stay. The swap passes when every job is within one level of the torch reference on under 0.01 % of bytes. The terrain A/B that gates D3 runs offline with the unditherer, so the game has no model switch. |
| D12 | **The notice.** One message box when the restorer turns itself off (it crashed, or the startup check failed), saying what happened and that it is tried again after a driver or game update. While it stays off, the render options' *Undithered assets* row reads "Off (driver)" (built shorter than the "Off (this driver)" asked for, which did not fit); picking On clears the record and tries once more, and another crash turns it off again. |
| D13 | **The self-test's reference is computed on the CPU at launch**, by a plain C version of the network from the weight files actually loaded, on a worker thread while the game starts (about 0.5 s; 1–2 s on an old CPU). It passes when every byte is within one level of it. Nothing has to be kept in step with the weight files, and the build needs no torch. |
| D14 | **What counts as the restorer crashing:** a crash on the render thread inside a restorer call; the GPU device lost while a frame carrying restorer work is unfinished (a fence that only times out counts when the device then reports itself lost; today a lost device takes the whole Vulkan renderer down, whoever caused it); the launch self-test failing. Nothing else is blamed. A wrong blame costs only undithering on that driver, and D12's row retries. |
| D15 | **D3's gate is a sheet the owner judges**, published as an artifact: eight maps picked by terrain type (water, snow, lava, metal, desert, grass, rock, the largest), each with the crop where tiny and full differ most and one ordinary crop, shown dithered, tiny, full and their difference. Every image opens in a lightbox that zooms to 100 % and beyond. The numbers go in this note beside the verdict; a map where the owner sees a loss sends terrain back to full. |

**D3's gate, the numbers the verdict was given on.** Whole maps, restored offline with the
unditherer (torch fp32, per-tile, as the game restores today); the difference is |tiny − full| per
byte, in levels of 255:

| map | terrain | mean | 99.9 % within | max | bytes that differ | PSNR (dB) |
|---|---|---|---|---|---|---|
| Two Continents | grass | 2.36 | 13 | 48 | 85 % | 38.1 |
| Brain Coral | water | 2.00 | 11 | 27 | 83 % | 39.6 |
| Anteer Strait | snow / ice | 2.60 | 13 | 27 | 87 % | 37.4 |
| Lava Alley | lava | 1.67 | 9 | 35 | 77 % | 41.0 |
| Core Prime Industrial Area | metal | 1.20 | 7 | 27 | 72 % | 43.8 |
| Painted Desert | desert | 1.99 | 11 | 36 | 83 % | 39.8 |
| Comet Catcher | rock | 1.13 | 5 | 12 | 72 % | 44.6 |
| Seven Islands | the largest | 0.80 | 5 | 24 | 62 % | 47.1 |

Most bytes differ, almost all by one or two levels, which is below what the eye separates when the
two are flipped at 100 %; the sheet's difference panel amplifies by 16 to show where. Measured cost on
the reference setup's RTX 4070 (torch, cuDNN): tiny 10.6 ns per pixel, full 65.1.

**When the restorer is off** (the self-test failed, or it crashed on this driver), Classic++ keeps
running and draws the original dithered art.

## Open

Nothing: every question has an answer. These follow from the code and the limits rather than from
a choice, and landing 2 is built on them:

- **The per-cell index is 32-bit.** Seven Islands' 303 MB atlas is roughly 65,000–69,000 cells of
  34×34 RGBA8, at or past what today's 16-bit `TILE_MAP` can index.
- **The atlas stays within the device's limits**: in today's 64-column layout Seven Islands would
  be about 35,000 px tall, past the 16384 most devices allow, so the neighbourhood atlas is laid
  out near square instead (*Landing 2, as built*).
- **The relaunch is TotalA.exe itself** with its original command line: no helper executable and
  no `rundll32`, both of which antivirus software watches.
- **The Windows test runs on** the Windows test setup's AMD Radeon R9 200-series card (2816 MB, a
  Vulkan 1.1 driver: fp32 only), with the machine's time shared with the mod-compatibility
  regression work.

## TODO

### Important: the tile-grid lines

Restored terrain shows a 1-px step on every 32-px tile line, most visibly on flat, smooth ground
such as the start area on King of the Hill. MEASURED 2026-09-26: the step across the tile line is
1.19× the median step, 1.03× one pixel in. The cause is the art itself: its dither restarts at
every tile, and the training data (`unditherer/synth.py`) dithers whole patches only, so the model
has never seen a pattern break that is not an edge. It is not a colour bias (low-passed retail
art gives a ratio of 1.01), and the seam fix does not remove it. The fix is a model trained on
per-tile-dithered data, whose bar is that step flat on held-out maps with no loss on the
evaluation set.

### Image-generation services as a source of training data

The owner plans to try image-generation services on TA's art, to see whether their undithered
versions of specific TA tiles can train the model on TA's own look.

### An upscaler (not part of this plan)

Undithering adds no resolution. Art sharper than the original needs super-resolution, which only
shows where one texel covers more than one screen pixel: zoomed in, and the HUD at a scale above
100 %. At zoom 1.0 at native resolution there is nothing to gain; the world's 2× internal target
is resolved back down.

- **Off-the-shelf models** were trained on photographs or anime, not palette-dithered art. Fed
  dithered input they enlarge the dither; fed our restored output they work, but the chain
  compounds both models' errors. Real-ESRGAN's compact network is the closest fit: a plain
  convolution stack like ours ending in a pixel shuffle, about 1.6× the full model's cost per
  input pixel by layer count [not measured]. Every model's licence has to be checked before it
  ships (many community models are non-commercial).
- **Our own joint undither + 2× model** is cheap to train: the synth pipeline already makes
  dithered/clean pairs from the licensed public corpus, and shrinking the clean image 2× before
  dithering turns them into super-resolution pairs. The per-tile dithering of the TODO above can
  go into the same retrain. It runs on the compute backend of landing 1.
- **Memory decides where it runs:** a 2× atlas is four times the size (140 MB on the median map,
  1.2 GB on Seven Islands), so it would upscale only what is on screen, when zoomed in. Sharpening
  a full 4K screen is about 0.2 s on the RTX 4070 [estimate].
- **Generative models** (a Stable Diffusion 1.x fine-tune, or smaller) do not run in the game:
  seconds per 512×512 tile, hours per map. Their results cannot ship either, since an upscaled
  Cavedog tile is still Cavedog art. In an RTS, invented rocks or cracks also mislead, because the
  ground looks different from how it plays. Their place is as an **offline teacher**: generate
  high-resolution TA art in the lab and train a small shipping network to copy it — the same shape
  as the image-generation idea above. That needs the owner's ruling on publishing weights trained
  on Cavedog art; today's weights are trained on the licensed public corpus only.

**Suggested order:** first a lab test of Real-ESRGAN's compact model at 2× on restored King of the
Hill crops and a unit sheet, against plain smooth scaling, to see the real upside before any
training. If it is worth it, our own joint model for the on-screen area when zoomed in, after
landing 2. Generative models only ever as a teacher.
