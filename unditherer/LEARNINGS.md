# Learnings: training a restorer for Total Annihilation's dither

The record of how `models/full` and `models/tiny` came to be: the recipe that
shipped, everything tried on the way, what failed, what had to change, and
what data was made. Dates are 2026-09-01 unless stated; hardware an RTX 4070
12 GB (driver 595), 32 CPU threads. The wiki page
`research/notes/undither.md` is the public write-up; this file is the lab
notebook behind it.

## 1. What "correct" means here

The game composes every frame into one 8-bit surface with a single 256-entry
palette. Where an artist wanted a colour the palette lacked, the tool
dithered: neighbouring pixels alternate between two entries and the eye
averages them. So a restoration is *correct* to the extent that it recovers
the colour the dither was standing in for, and it can be checked without
ground truth: re-quantise the restored image with the same kind of dither
and it should land back on the original pixels. That re-quantisation test
(`--consistency` in the CLI; `synth.dither_consistency`) turned out to be the
diagnostic that found the shipped model's one real flaw (section 3.6).

Two consequences shaped everything below:

- **Dither is mean-preserving.** Error diffusion pushes each pixel's
  quantisation error onto its neighbours, so over any block of a few pixels
  the average of the dithered image equals the average of the source. A
  model that shifts the local mean is not undithering; it is repainting.
- **Nobody knows which quantiser Cavedog used.** The frames show
  error-diffusion-like patterns (strict 2×2 checkerboards are under 0.3 % of
  windows), not ordered dither. Training therefore mixes several quantisers
  rather than betting on one.

## 2. The recipe that shipped

Every step is a script under `unditherer/`; `pipeline.py` with
`configs/shipped.json` runs them in order.

1. **Corpus** (`fetch_data.py`): 1,548 CC0 true-colour textures, 1.39 GB
   kept of ~4.6 GB transferred. 496 ambientCG materials (categories Ground,
   Rock, Grass, Snow, Ice, Lava, Gravel, Moss, Metal, Rust, Concrete,
   Asphalt; the 1K-JPG zip of each, only `*_Color.jpg` kept), 527 Poly Haven
   textures (terrain, rock, sand, gravel, snow, aerial, moss, the moon
   collection, brick, concrete, cobblestone, road, metal, asphalt, tiles,
   roofing; 1k diffuse JPG), 525 painted 512×512 tiles from Screaming Brain
   Studios' Tiny Texture Packs 1–3 on OpenGameArt. Throttled 0.5 s per
   request, resumable, manifest records source, licence, URL, tags and
   authors. Licences verified on the sources' pages: CC0 1.0 throughout
   (`CREDITS.md`).
2. **Palette** (`ta-palette.json`): 256 entries, 242 distinct, read from the
   captured frames. All fifteen frames share it, so it is the game's.
3. **Pair synthesis** (`synth.make_pair`, on the fly in DataLoader workers,
   ~1.1 ms per 96² pair): random crop at a random 1–6× downscale (BOX or
   LANCZOS), rot90/flip, then with p = 0.7 brightness 0.7–1.25, contrast
   0.8–1.2, **saturation 0.6–2.2**; **hue rotated through the full circle
   with p = 0.5**; quantised to the palette with a mode drawn per sample from
   `fs, fs, fs, nearest, ordered4, ordered8` — Floyd–Steinberg via PIL (C,
   non-serpentine, approximate palette cache), Bayer 4×4 / 8×8 with amplitude
   U(16, 48), or nearest colour with no dither (that mode *is* banding, so the
   one model also debands). The dither runs on a crop with an 8 px margin so
   error diffusion has warmed up before the patch. Input = palette RGB of the
   indices; target = the clean crop; also `is_dither` (0 for nearest) for the
   loss below.
4. **Model** (`model.py`): DnCNN-shaped residual CNN, output = input −
   net(input), last conv zero-initialised so training starts from the
   identity. Full: depth 12, 64 channels, BatchNorm, 373,443 params, 25 px
   receptive field, 1.5 TFLOPs per 1080p frame (2 FLOPs per multiply-add),
   0.17 s on the 4070. Tiny: depth 6, 24 channels, 22,251 params, 13 px,
   91 GFLOPs, 57 ms unoptimised.
5. **Training** (`train.py`): L1 loss, AdamW (weight decay 1e-5), OneCycle
   cosine peaking at 1e-3 with 5 % warm-up, batch 64 of 96² patches, fp16
   autocast + GradScaler, gradient clip 1.0, 40,000 steps = 2.56 M fresh
   patches, 10 workers, validation every 2,000 steps on 256 held-out patches
   (5 % of source images held out by a seeded split — 77 images). Full:
   62 min; tiny: 49 min, both at once on the one GPU, ~800 patches/s.
6. **Fine-tune** with the block-mean consistency term: `--init <plain
   checkpoint> --mean-weight 1.0`, fresh optimiser and schedule, full at
   lr 2e-4, tiny at 3e-4, 8,000 steps each (13 / 10 min). The term is the L1
   distance between 8×8 average-pooled output and average-pooled *input*,
   applied only to dithered samples.
7. **Evaluate** (`evaluate.py`) on one fixed set: 512 patches, seed 1234.
   Necessary because each run's own validation set changes whenever the
   augmentation changes.
8. **Export and bake**: `pipeline.py export` writes `models/*.pt` with the
   provenance inside the checkpoint and single-file ONNX (opset 17, parity
   3 × 10⁻⁷ against torch); `bake` restores the fifteen frames to lossless
   WebP for the wiki viewer. The bakes (21 MB) are gitignored; the wiki build
   regenerates them from the shipped model when missing or stale.

### Numbers

Fixed synthetic set, PSNR against the clean target (higher is better):

| | dithered input | tuned bilateral | tiny 6×24 | full 12×64 |
|---|---|---|---|---|
| plain L1 | 24.43 dB | 27.04 dB | 30.55 | 31.72 |
| + block-mean consistency (**shipped**) | | | 29.79 | 30.48 |

Each run's own validation set (not comparable across augmentation changes):
full 31.85, tiny 30.54, full-mc 30.17, tiny-mc 29.67, the dead ends full-ft
29.91 and tiny-ft 28.32 (on a different val set: 22.87 / 25.77 baselines).

On the game frames there is no ground truth. Shipped full model: edge
retention 0.99–1.00 on all fifteen, mean colour shift 0.05–1.42 levels
(float64; the per-frame table is in section 8).
Dither-consistency (re-quantise with Floyd–Steinberg, over the terrain area;
histogram overlap / exact-index match):

| frame | tuned bilateral | full CNN |
|---|---|---|
| Water World | 0.952 / 0.714 | 0.995 / 0.882 |
| Green planet | 0.798 / 0.330 | 0.865 / 0.529 |
| Lava | 0.847 / 0.630 | 0.911 / 0.817 |
| Crystal | 0.783 / 0.420 | 0.775 / 0.438 |

(The CLI's `--consistency` measures over the whole frame, HUD included, so its
absolute numbers are lower; the ordering is the same.)

## 3. What was tried, in order

1. **Smoke test — 300 steps, 170 images.** Already 31.6 dB against the
   bilateral's 29.0 on that run's validation set; throughput ~800 patches/s.
   Bug found: the manifest was only written at the end of the fetch, so a
   training run started mid-fetch saw no corpus. `corpus_files()` now falls
   back to globbing `images/`, and the fetch writes the manifest every 25
   images.
2. **Interim 1 — 6,000 steps, 288 images, no hue augmentation.** 33.9 dB on
   its validation set; grass, rock, sand and metal looked right. **Lush
   shifted 13.8 levels** toward a dull red-green; **Water World was
   half-resolved with a 9.7-level hue drift.** Diagnosis: a corpus of browns
   and greys never visits the saturated green and blue regions of the
   palette, so the model had no idea what dither looks like there.
3. **Interim 2 — hue rotation added (p = 0.5), 600 images.** Lush 3.8, Water
   5.5 and the water dither resolved cleanly; Crystal 0.33, Green planet
   0.86. **Kept.** Lesson: a palette-inverse model has to be shown the whole
   palette, not just the colours its training subject happens to have.
4. **Full 40k and tiny 40k runs, hue + saturation 0.6–2.2.** 31.72 / 30.55 dB
   on the fixed set. Every frame good except **Water World: a 12.3-level
   shift** (brighter, greener blue); Lava 4.4, Lush 2.0 (tiny: 9.7 / 1.5 / 2.1).
5. **Vivid-saturation push — dead end.** HSV saturation pushed toward 255
   with p = 0.4 in augmentation, then an 8k fine-tune from each plain
   checkpoint (`runs/full-ft`, `runs/tiny-ft`). **Worse on the water**: full
   12.3 → 19.1 levels, tiny 9.7 → 11.6 (at the time it read as "no effect"
   through the float32 metric of section 5; the exact numbers say harmful).
   Removed from the augmentation. The knob survives as `--vivid-p` (default 0)
   so the experiment can be re-run rather than re-discovered. The code is the
   removed code verbatim, recovered from the session transcript on 2026-09-02:
   inside the hue-rotation branch, with probability 0.4, `S' = 255 − (255 − S)
   · keep` with `keep ~ U(0, 0.5)`, i.e. saturation moved 50–100 % of the way
   to full. Because it sat inside the hue branch it touched 0.5 × 0.4 = 20 %
   of crops.
6. **Re-quantisation diagnosis.** The bilateral's water re-dithers into the
   frame at 0.95 histogram overlap; the CNN's at 0.56 under Floyd–Steinberg
   but **0.80 under nearest-colour**. So the model was reading a 66/22/11 %
   three-blue interleave as *banding* (something to smooth toward a colour
   prior learned on banded regions) rather than as *dither* (something whose
   local mean is the answer). A training-signal problem, not a data problem:
   nothing in a per-pixel L1 loss says that the mean must be preserved.
7. **Block-mean consistency loss — the fix.** `--mean-weight 1.0` on
   dithered samples only, 8k fine-tune from the plain-L1 checkpoints:
   **Water World 12.3 → 0.53 levels, Lava 4.4 → 1.4, every frame ≤ 1.42**
   (tiny: 9.7 → 0.81, all ≤ 1.17), consistency above the bilateral on every
   frame tested, at a cost of
   1.2 dB (full) / 0.8 dB (tiny) of synthetic PSNR. The cost is expected and
   honest: some synthetic targets are gamut-clipped cases (a source colour
   outside what the palette can average to), where preserving the dithered
   mean is "wrong" per the target but right per the frame. **Shipped.**

Two general lessons: show a palette-inverse model the whole palette, and tell
it that dither is mean-preserving.

## 4. Corpus notes

- **Why photographs and painted tiles, not the game's own art.** TA's tiles
  are quantised; there is no clean target to learn from. Training pairs are
  synthesised from true-colour sources, which makes the ground truth exact.
  The cost is a domain gap in *subject*; the 1–6× downscale closes most of
  the gap in *scale* (fleck-scale detail lands at the pixel scale, like a
  32×32 painted tile), and the hue rotation closes the gap in *colour*.
- **The painted Tiny Texture Packs matter more than their share.** They are
  the only hand-painted material in the set, and painted single-pixel grit
  (Green planet grass, Desert sand, the Urban tileset) is exactly what the
  spec's 7×7 bilateral destroys and what the model must learn *not* to
  average. The wiki page's index-count table is the evidence that those
  frames are texture, not dither.
- **Saturation range 0.6–2.2, not the usual 0.8–1.2.** TA's water, lava and
  crystal sit far outside photographic saturation; the wide range plus hue
  rotation is what put training samples in those palette regions. The
  further push toward full saturation (dead end #5) added nothing because
  the missing signal was about the mean, not the colour distribution.
- **No image was edited or duplicated on disk.** Every colour change in the
  data path (saturation 0.6–2.2, hue rotation, the vivid push) was applied on
  the fly to random crops of every image, with the probabilities above; the
  corpus directory holds exactly the 1,548 files the manifest lists and
  nothing derived from them. The dataset got "more complete" in two other
  ways: it grew from 170 to 288 to 600 to 1,548 images as the fetch ran
  (interim runs 1 and 2 trained on the partial corpus), and hue rotation put
  every texture into every region of the palette.
- **The exact list ships with the package.** `unditherer/corpus/manifest.json`
  is the record of every image (source, id, page, licence, tags, authors,
  bytes); `python -m unditherer.fetch_data` rebuilds the corpus from it by id,
  and `--verify` checks a corpus against it. Category listings on the sources
  change over time; the manifest does not.
- **Not in the corpus:** unit sprites. They are rendered at run time through
  lighting tables rather than dithered, so the model has not seen that
  statistic; nearest-colour banding is in the mix, which is the closest
  thing.
- **What was made, and where:** `.data/undither-train/` at the main checkout
  root (gitignored, shared by worktrees; 1.4 GB): `images/`, `manifest.json`
  (the same list as the shipped one), `ATTRIBUTION.md`, `raw/` (the OGA
  zips), `contact-sheet.jpg`, and
  `runs/{full,full-ft,full-mc,tiny,tiny-ft,tiny-mc}/` with `best.pt`,
  `last.pt`, `log.json`. The shipped checkpoints are `full-mc/best.pt`
  (step 4000) and `tiny-mc/best.pt` (step 4000), copied with provenance to
  `unditherer/models/`.

## 5. Bugs and gotchas fixed along the way

- PIL hands out read-only NumPy arrays; `synth.random_crop` returns
  `np.array(...)` copies because torch wants writable memory.
- `torch.onnx.export` on torch 2.13 needs `onnxscript` and, with the default
  dynamo exporter, wrote the full model's weights to an external `.data`
  sidecar. `dynamo=False` produces a single file; opset 17 is upgraded to 18
  with a warning, harmless. The export now refuses to leave a sidecar.
- `--resume` continues the schedule position; `--init` is the fine-tune
  path (weights only). Mixing them up restarts a cosine schedule at the
  wrong place.
- Background `… | grep | tail` pipes buffer everything until exit; read
  `runs/*/log.json` for progress instead.
- The fetch's User-Agent is a plain project string. Early on it carried a
  personal email address by mistake; do not do that.
- PIL's `quantize(palette=…)` uses an approximate colour cache, fine for
  synthesis but not for scoring: the consistency metric now finds the
  source's own palette indices with an exact nearest-colour lookup.
- `split()` held out `max(16, 5 %)` images, which on a toy corpus left no
  training images at all; it now keeps at least one on each side while
  reproducing the 77-image hold-out of the real corpus exactly.
- The pipeline's dry run skips finished experiments just like a real run;
  pass `--force` to see the training commands.
- **The mean-colour-shift metric was wrong before 2026-09-02.** `qa()` and
  the bake averaged 2 million pixels in float32 with `mean((0, 1))`, which
  NumPy cannot sum pairwise on that axis pair, so the answer drifted by up to
  four levels and even depended on the array's memory layout: the same
  Water World output scored 0.96 from one code path and 4.03 from another
  (truth: 0.53). Fixed with float64 means (`classical.mean_colour`), with a
  regression test on a 1080p frame. Every shift number in this file and in
  the wiki was recomputed from the surviving checkpoints; the interim-run
  figures in section 3 (items 2 and 3) come from checkpoints that were not
  kept and are the float32 readings, so treat them as approximate. The
  qualitative conclusions did not move; one got sharper (dead end #5).

## 6. Ideas not done

- Other error-diffusion kernels (Jarvis, Stucki, Atkinson, serpentine
  Floyd–Steinberg): need a numba/Cython loop; PIL only does FS.
- Exclude or down-weight gamut-clipped sources so the mean-preservation term
  and the L1 target stop disagreeing (would recover some of the 1.2 dB).
- An SSIM or perceptual term; larger patches; longer training (the 40k curve
  was still rising slowly at 31.85 dB).
- Self-supervised consistency on the game's own TNT tiles: re-quantise the
  output and demand the input's indices. That is domain data with no clean
  target required.
- The tiny model as a fragment shader in the renderer; an in-browser ONNX
  runtime for the viewer instead of baked WebP.
- The deband gate (`step > 8`) mis-fires on kept texture; a gate on run
  length rather than neighbour distance would not.
- The whole-map before/after viewer that was requested and interrupted:
  render a map's terrain from its `.tnt`, split-slider, tileset strip,
  net selector. The TNT header layout and the HPI parser are noted in the
  session handoff of 2026-09-01.

## 7. Reproducing

```bash
PY=.venv-undither/bin/python
$PY -m unditherer.pipeline --config shipped --stages fetch        # ~1 h of polite downloading
$PY -m unditherer.pipeline --config shipped --parallel            # ~2.5 h on a 4070: full+tiny, then the -mc fine-tunes
$PY -m unditherer.pipeline --config shipped --stages bake         # wiki frames, shots.json, site
$PY -m unditherer.pipeline --config shipped -e full-ft,tiny-ft    # the dead ends, if you must
```

`runs/results.md` is the table the pipeline writes; `runs/pipeline.jsonl` is
the append-only log of what ran when. Re-running the evaluate stage on the
recorded checkpoints reproduces the fixed-set numbers above to the second
decimal, and re-baking the fifteen frames with the shipped `full` model
reproduces the committed WebP files byte for byte (checked 2026-09-02).

## 8. Mean colour shift per frame (float64, levels)

Largest per-channel change of the frame's mean colour after restoration, for
every surviving checkpoint and the tuned bilateral. `full-mc` and `tiny-mc`
are the shipped models; `-ft` are the vivid-saturation dead ends.

| frame | bilateral tuned | full | full-ft | **full-mc** | tiny | tiny-ft | **tiny-mc** |
|---|---|---|---|---|---|---|---|
| Acid | 0.22 | 1.23 | 1.52 | 0.97 | 1.67 | 1.40 | 0.75 |
| Archipelago | 0.37 | 1.56 | 1.72 | 1.15 | 1.22 | 0.90 | 0.18 |
| Crystal | 0.70 | 0.20 | 0.32 | 0.08 | 0.33 | 0.37 | 0.07 |
| Desert | 0.62 | 0.57 | 0.67 | 0.20 | 1.15 | 1.23 | 0.06 |
| Green planet | 1.06 | 0.24 | 0.24 | 0.13 | 0.20 | 0.32 | 0.06 |
| Ice | 0.27 | 0.74 | 0.61 | 0.39 | 0.69 | 0.78 | 0.61 |
| Lava | 0.08 | 4.44 | 4.77 | 1.42 | 1.54 | 1.62 | 0.65 |
| Lunar | 0.13 | 0.96 | 1.23 | 0.05 | 0.87 | 0.84 | 0.05 |
| Lush | 0.07 | 2.00 | 3.00 | 1.33 | 2.07 | 2.17 | 1.17 |
| Metal | 0.08 | 0.26 | 0.38 | 0.07 | 0.71 | 0.80 | 0.04 |
| Red Planet | 1.91 | 0.75 | 0.50 | 0.14 | 0.61 | 0.63 | 0.05 |
| Slate | 0.67 | 0.18 | 0.33 | 0.12 | 0.20 | 0.28 | 0.05 |
| Urban | 1.11 | 0.50 | 0.43 | 0.10 | 0.25 | 0.40 | 0.06 |
| Water World | 1.30 | 12.32 | 19.06 | 0.53 | 9.69 | 11.56 | 0.81 |
| Wet Desert | 0.16 | 1.89 | 2.35 | 0.63 | 1.55 | 1.69 | 0.56 |
| **max** | 1.91 | 12.32 | 19.06 | **1.42** | 9.69 | 11.56 | **1.17** |

Read down the Water World row: the plain-L1 models drift, the vivid push
makes it worse, the block-mean term fixes it. Read down the bilateral
column: the kernel is honest about colour everywhere but at the same time
it is the one leaving dither behind on textured frames, which is what the
consistency test and the PSNR table measure.
