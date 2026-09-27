# The fp32 compute restorer — the plan

**Written** 2026-09-27. **Planned, nothing built in the game.** The design is being settled one
question at a time with the owner; this page holds the decisions taken so far, the questions still
open, and the TODO list of the work. fp16 (cooperative matrices) is deferred.

The restorer is the learned unditherer Classic++ runs on the game's art ([Classic and Classic++
renderers](renderers.html), [Undithering screenshots](undither.html)). Today it is a fragment-shader backend
(`tagpu_vk_restore.c`) under the API-free core (`tagpu_restore_core.c`): job queues, batches, a
GPU-time budget per frame, and the pass sequence fill → one pass per layer → out. This work
replaces that backend with Vulkan compute, then uses the speed for terrain restored across tile
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

## Decisions

| # | decision |
|---|---|
| D1 | **Two landings.** Landing 1 swaps the backend: fp32 compute for all six jobs (terrain, features, effects, units, UI, pictures) under the existing core, the fragment backend deleted. It is tested on Windows too, sharing the Windows test machine with the mod-compatibility regression work. Landing 2 is the terrain seam fix. |
| D2 | **A self-test at every launch** runs the same pipelines on a synthetic probe (nothing from the original game) and checks the result against D13's reference. A crash attributed to the restorer records `off` for that device and driver, and the game relaunches at once with a notice. A new driver version gets a fresh try. |
| D3 | **tiny for terrain, full for everything else** (units, features, effects, UI). Terrain is the volume; tiny (6 layers × 24 channels, 22k MACs per pixel) costs about a tenth of full (12 × 64, 372k). It is gated on a wider terrain A/B first (water, snow, lava and metal maps); any visible loss sends terrain back to full. The switch goes in with landing 2: landing 1 changes no picture. |
| D4 | **The browser lab's `restore=glsl` lane is dropped.** The lab keeps `restore=pack`. |
| D5 | **A neighbourhood atlas for terrain:** one restored copy per distinct tile graphic *plus its 8 neighbours*, drawn through a per-cell index. King of the Hill: 2,978 graphics, 69,958 spots, 26,827 keys, 118 MB. Median map 35 MB, Two Continents 193 MB, Seven Islands (the largest) 303 MB, against 14–51 MB for today's per-graphic atlas. A map whose atlas does not fit the GPU's memory budget keeps today's atlas, seams included. |
| D6 | **The tile-grid lines are left for now** — see the TODO below. |
| D7 | **No quick per-tile pass first.** Terrain restores straight into the neighbourhood atlas, spots on screen first; a spot not restored yet draws dithered, as it does during today's reveal. |
| D8 | **The GPU budget during play:** spots on screen get the full budget (12 ms per frame); the rest of the map restores at a fixed trickle of about 2 ms. Spots scrolled onto the screen move to the front. |
| D9 | **No disk cache.** The terrain is restored every time a map is played. |
| D10 | **Whether the atlas fits** is decided before allocating. Its size is exact once the map loads (keys × cell size). The driver's memory-budget query gives the memory still free for the game; a driver without that query counts a quarter of the GPU's device-local memory as free. The atlas fits when it takes at most half of that free memory. An allocation the driver still refuses falls back as well. |
| D11 | **Settings and the bar for landing 1.** The `fp16`, `nk=` and `tiny` settings are removed; `budget=` and `log` stay. The swap passes when every job is within one level of the torch reference on under 0.01 % of bytes. The terrain A/B that gates D3 runs offline with the unditherer, so the game has no model switch. |
| D12 | **The notice.** One message box when the restorer turns itself off (it crashed, or the startup check failed), saying what happened and that it is tried again after a driver update. While it stays off, the render options' *Undithered assets* row reads "Off (this driver)"; picking On clears the record and tries once more, and another crash turns it off again. |
| D13 | **The self-test's reference is computed on the CPU at launch**, by a plain C version of the network from the weight files actually loaded, on a worker thread while the game starts (about 0.5 s; 1–2 s on an old CPU). It passes when every byte is within one level of it. Nothing has to be kept in step with the weight files, and the build needs no torch. |
| D14 | **What counts as the restorer crashing:** a crash on the render thread inside a restorer call; the GPU device lost while a frame carrying restorer work is unfinished (today a lost device takes the whole Vulkan renderer down, whoever caused it); the launch self-test failing. Nothing else is blamed. A wrong blame costs only undithering on that driver, and D12's row retries. |
| D15 | **D3's gate is a sheet the owner judges**, published as an artifact: eight maps picked by terrain type (water, snow, lava, metal, desert, grass, rock, the largest), each with the crop where tiny and full differ most and one ordinary crop, shown dithered, tiny, full and their difference. Every image opens in a lightbox that zooms to 100 % and beyond. The numbers go in this note beside the verdict; a map where the owner sees a loss sends terrain back to full. |

**When the restorer is off** (the self-test failed, or it crashed on this driver), Classic++ keeps
running and draws the original dithered art.

## Open

Nothing: every question has an answer. These follow from the code and the limits rather than from
a choice, and the implementation takes them as given:

- **The per-cell index is 32-bit.** Seven Islands' 303 MB atlas is roughly 65,000–69,000 cells of
  34×34 RGBA8, at or past what today's 16-bit `TILE_MAP` can index.
- **The atlas pages within the device's limits** (a 2D array texture sized by
  `maxImageDimension2D` and `maxImageArrayLayers`): in today's 64-column layout Seven Islands would
  be about 35,000 px tall, past the 16384 most devices allow.
- **The per-driver record lives in `impure.cfg`,** not in a new file.
- **The relaunch is TotalA.exe itself** with its original command line: no helper executable and
  no `rundll32`, both of which antivirus software watches.
- **The Windows test runs before landing 1 lands**, on the Windows test setup's AMD Radeon R9
  200-series card (2816 MB, a Vulkan 1.1 driver: fp32 only), with the machine's time shared with
  the mod-compatibility regression work.

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
