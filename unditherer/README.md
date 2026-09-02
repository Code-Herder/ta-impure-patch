# unditherer

Restore Total Annihilation's 256-colour, dithered frames and textures to the
true colour they were standing in for. One folder holds everything: the
classical filters, two trained restorer networks with their provenance, the
training pipeline that made them, the experiment log, the corpus credits, a
command-line tool that runs every method, and the tests.

| Where | What |
|---|---|
| `cli.py` | the `unditherer` command (`python -m unditherer …`) |
| `classical.py` | bilateral undither (presets spec / tuned / tight / split), deband, enhance, QA |
| `restore.py` | one image through the stages, classical or learned |
| `model.py`, `infer.py` | the residual CNN; loading `.pt` / `.onnx`, tiling, wrap padding, ONNX export |
| `models/` | **the shipped models**: `full.{pt,onnx}` (12×64, 373 k params), `tiny.{pt,onnx}` (6×24, 22 k), `models.json` provenance |
| `synth.py`, `data.py` | pair synthesis (true colour → TA palette + dither), corpus split, streaming loader |
| `train.py`, `evaluate.py`, `fetch_data.py` | the three steps, each runnable on its own |
| `corpus/` | `manifest.json`: the exact 1,548-image training set (ids, pages, licences, authors); `ATTRIBUTION.md` |
| `pipeline.py`, `configs/` | fetch → train → eval → export → bake → report from one config |
| `LEARNINGS.md` | what was tried, what failed, what shipped, and why |
| `CREDITS.md`, `LICENSES/` | corpus licences (all CC0 1.0), links, thank-yous |
| `credits.py` | per-asset attribution list from the fetch manifest |
| `ta-palette.json` | the game's 256-entry palette, read from its frame buffer |
| `tests/` | 85 tests; `pytest unditherer/tests` |

## Install

```bash
python3 -m venv .venv-undither && .venv-undither/bin/pip install -r unditherer/requirements.txt
# learned presets on CPU need only onnxruntime (in requirements.txt);
# for CUDA inference, training and export add torch, onnx, onnxscript:
.venv-undither/bin/pip install torch onnx onnxscript
```

Everything runs as a module from the repository root: `python -m unditherer …`.
In this repository the environment already exists at
`<repo>/.venv-undither` (Python
3.12, torch 2.13 + CUDA 13, OpenCV 5, onnxruntime 1.29).

## The command-line tool

```
python -m unditherer restore INPUT [INPUT …] [-o OUT] [options]
python -m unditherer analyze INPUT [--json]
python -m unditherer presets [--json]
python -m unditherer models  [--json]
```

`restore` reads indexed PNGs (the game's frames and textures) or any RGB
image, measures the dither amplitude `q` and the palette band step, runs the
stages, and writes the result. Default output: `<name>.undithered.png` next
to the input. With several inputs, `-o` is a directory. `.webp` outputs are
lossless.

### Parameters

| Option | Default | Meaning |
|---|---|---|
| `-p, --preset` | `tuned` | `spec` 7×7 disc · `tuned` 5×5 disc · `tight` 3×3 cross · `split` 5×5, luma/chroma metric · `learned` full CNN · `learned-tiny` |
| `--kernel 3x3\|5x5\|7x7` | preset's | disc size (sets σ<sub>s</sub> = 0.9 / 1.2 / 1.8; OpenCV radius = round(1.5 σ<sub>s</sub>)) |
| `--sigma-s F` | preset's | σ<sub>s</sub> directly (exclusive with `--kernel`) |
| `--metric rgb-l1\|split` | preset's | range metric: OpenCV's \|Δr\|+\|Δg\|+\|Δb\| against σ<sub>r</sub>, or luma at 0.31 σ<sub>r</sub> / chroma at 1.15 σ<sub>r</sub> (aliases: `l1`, `rgbl1`, `luma-chroma`) |
| `--passes N` | 2 | bilateral passes |
| `--k F` | 1.3 | σ<sub>r</sub> = k · q. Below 1 the dither survives as an "edge"; above ~2 silhouettes melt |
| `--strength F` | 1.0 | blend original → filtered (the viewer's Strength slider) |
| `--q F` | measured | override the dither amplitude |
| `--undither on\|auto\|off` | `on` | `on` always (what the viewer does); `auto` only when the lag-1 detector fires (restore.py's rule); the CNN always runs unless `off` |
| `--deband auto\|on\|off` | `auto` | `auto` is restore.py's gate: band step after undithering > 8 (and residual HF energy > 2). Off for the learned presets, which deband themselves |
| `--deband-sigma F` | 6 | blur across single-step contours, px |
| `--enhance` | off | procedural local contrast + vibrance (`--enhance-amount/-clarity/-vibrance`). Invents; last and optional |
| `--wrap auto\|yes\|no` | `auto` | tileable borders: wrap padding for the kernels and the CNN. `auto` measures opposite edges |
| `--model M` | per preset | learned presets: `full`, `tiny`, or a path to a `.pt` / `.onnx` |
| `--backend auto\|torch\|onnx` | `auto` | torch on CUDA if available, else onnxruntime, else torch on CPU |
| `--device auto\|cuda\|cpu` | `auto` | torch device |
| `--tile N`, `--overlap N` | 1088, 32 | tiled inference above `tile` px; overlap must exceed the receptive radius (12 for `full`, 6 for `tiny`) |
| `--report` | off | write `<output>.json`: analysis, decisions, kernel, model, QA, timing |
| `--json` | off | print the same report to stdout |
| `--consistency` | off | add the re-quantisation test: how well the output dithers back into the source (histogram overlap, exact-index match) |
| `--format png\|webp` | png | output format when `-o` is a directory or omitted |
| `-q, --quiet` | | no per-file summary line |

Every colour-key pixel of an indexed image (its `transparency` index) is
inpainted before filtering and restored to alpha 0 on output, so sprites keep
their cut-outs.

### Examples

```bash
S=research/notes/assets/undither/shots          # the fifteen 1080p captures

# the viewer's default: tuned 5×5, two passes, RGB L1, forced on
python -m unditherer restore $S/green-planet.png -o /tmp/green.png

# the spec's 7×7 kernel, the 3×3 cross, the luma/chroma split
python -m unditherer restore $S/green-planet.png --preset spec  -o /tmp/spec.png
python -m unditherer restore $S/green-planet.png --preset tight -o /tmp/tight.png
python -m unditherer restore $S/green-planet.png --preset split -o /tmp/split.png

# hand-built kernel: 5×5 disc, RGB L1, three passes, σr = 1.1 q, half strength
python -m unditherer restore $S/desert.png --kernel 5x5 --metric rgb-l1 --passes 3 --k 1.1 --strength 0.5

# the learned restorer (full 12×64) — torch on the GPU if there is one, else onnxruntime
python -m unditherer restore $S/water-world.png --preset learned -o /tmp/ww.png
# force the ONNX path, or the tiny model, or your own checkpoint
python -m unditherer restore $S/water-world.png --preset learned --backend onnx
python -m unditherer restore $S/water-world.png --preset learned-tiny
python -m unditherer restore $S/water-world.png --preset learned --model .data/undither-train/runs/full/best.pt

# a whole directory, lossless WebP, with a JSON report per file
python -m unditherer restore $S/*.png -o /tmp/restored --format webp --report

# restore.py's own decision tree (detector-gated) and the consistency metric
python -m unditherer restore $S/lush.png --undither auto --consistency --json

# a tileable texture: wrap borders and no enhancement
python -m unditherer restore rock_tile.png --wrap yes

# just the measurements
python -m unditherer analyze $S/*.png
python -m unditherer presets
python -m unditherer models
```

A summary line per file goes to stderr:

```
water-world.png: q=33.3 step=44.9->3.6 tuned 5x5 disc x2 l1 sigma_r=43.3 undither=True deband=False edges=0.99 hf_red=0.94 shift=1.30 1.69s -> /tmp/ww.png
```

`edges` is edge retention (Canny edges of the input that survive), `hf_red`
the reduction of high-frequency energy in flat regions, `shift` the largest
per-channel change of the mean colour in levels. These are the numbers the
wiki page quotes; `tools/undither/prep.py` computes the same ones for the
viewer.

### From Python

```python
from unditherer.restore import Options, restore_rgb, restore_file
out, report = restore_rgb(rgb_u8, Options(preset="tuned", passes=3))
out, report = restore_rgb(rgb_u8, Options(preset="learned", backend="onnx"))
restore_file("frame.png", "frame.out.webp", Options(preset="split"), report_path="frame.json")

from unditherer.infer import LearnedRestorer      # the bare network
LearnedRestorer("tiny").restore(rgb_u8)           # picks a backend; .info() says which
```

## The models

| model | net | params | receptive field | FLOPs / 1080p frame | fixed-set PSNR | RTX 4070 | files |
|---|---|---|---|---|---|---|---|
| `full` | 12 layers × 64 ch | 373,443 | 25 px | 1.5 T | 30.48 dB | 0.17 s | `full.pt` 1.5 MB, `full.onnx` 1.5 MB |
| `tiny` | 6 × 24 | 22,251 | 13 px | 91 G | 29.79 dB | 57 ms unoptimised | `tiny.pt` 99 KB, `tiny.onnx` 91 KB |

Both are DnCNN-shaped residual CNNs (output = input − net(input), BatchNorm,
ReLU) trained on pairs synthesised from CC0 textures with the game's own
palette, then fine-tuned with a block-mean consistency term. On the same fixed
set of 512 held-out synthetic patches the dithered input scores 24.43 dB and
the tuned bilateral 27.04 dB. `models/models.json` records, per model: the
run it came from, step, both PSNRs, the full training recipe and augmentation,
the ONNX-vs-torch parity (3 × 10⁻⁷) and file sizes. The `.pt` files carry the
same provenance inside the checkpoint dict; the `.onnx` files carry it in
`metadata_props`. `LEARNINGS.md` explains every choice.

The models are deterministic and 1× (no upscaling, no generation). They
decide per pixel whether a fleck is dither or painted texture; on saturated
water and lava the shipped models keep the mean colour within a level or two
of the source, which the plain-L1 versions did not (see the learnings).

## Training pipeline

### The corpus

The images are 1.4 GB, so they live outside git at
`<main checkout>/.data/undither-train/` (override with `UNDITHERER_DATA=…`).
What ships in the package is the exact list: `corpus/manifest.json` records
every one of the 1,548 images (source, id, page URL, licence, tags, authors,
byte size), and `corpus/ATTRIBUTION.md` is the credit list generated from it.

```bash
python -m unditherer.fetch_data --dry-run     # what is present locally, what would be fetched
python -m unditherer.fetch_data               # rebuild the corpus from the manifest, by id; resumable
python -m unditherer.fetch_data --verify      # every listed file present with its recorded size
python -m unditherer.fetch_data --discover    # a NEW corpus from the sources' category listings
```

Manifest mode asks each source for exactly the listed ids (ambientCG through
its API's id filter in batches, Poly Haven per asset, the OpenGameArt zips by
file name), so it reproduces the training set even after the sites' category
listings change. Anything a source has since removed is reported and written
to `fetch-missing.json`. Requests are throttled to one every half second;
the full fetch moves about 4.6 GB. The data directory then holds:

```
.data/undither-train/
  images/{ambientcg,polyhaven,sbs-tiny}/   1,548 CC0 textures, 1.4 GB
  manifest.json                            the same list, byte sizes refreshed from disk
  ATTRIBUTION.md                           generated per-asset credit list
  raw/                                     the OpenGameArt zips
  runs/<experiment>/{best.pt,last.pt,log.json}
  runs/eval.json  runs/results.md  runs/pipeline.jsonl
```

No image in the corpus was edited or duplicated: every colour change in the
training data (saturation, hue, the vivid push) is on-the-fly augmentation of
random crops, listed under Tweaking below.

The whole recipe is one config, `configs/shipped.json`, and one runner:

```bash
python -m unditherer.pipeline --config shipped --dry-run          # what would run
python -m unditherer.pipeline --config shipped --stages fetch     # the corpus: ~4.6 GB transferred, 0.5 s/request
python -m unditherer.pipeline --config shipped                    # train, eval, export, report
python -m unditherer.pipeline --config shipped --stages bake      # the 15 wiki frames + shots.json + site
python -m unditherer.pipeline --config shipped -e full,full-mc --force --parallel
```

The bakes the `bake` stage writes (15 lossless WebP frames, 21 MB) are not
kept in git: `research/build_wiki.py` regenerates them with `models/full`
whenever they are missing or older than the model, so a fresh clone gets them
on its first wiki build (`--rebake` forces it, `--no-bake` skips it).

Stages: `fetch` (CC0 corpus + attribution file), `train` (each experiment in
dependency order; `init` names the experiment whose `best.pt` seeds a
fine-tune; finished experiments are skipped unless `--force`; `--parallel`
runs independent ones together), `eval` (every experiment on the one fixed
validation set → `runs/eval.json`), `export` (checkpoint + single-file ONNX +
provenance into `models/`), `bake` (the learned frames for the wiki viewer,
`prep.py`, `build_wiki.py`), `report` (`runs/results.md`, the table below).

The shipped run, on an RTX 4070 with 10 loader workers:

| experiment | net | steps | peak lr | mean-weight | init | minutes | fixed-set dB |
|---|---|---|---|---|---|---|---|
| full | 12×64 | 40,000 | 1e-3 | 0 | | 62 | 31.72 |
| tiny | 6×24 | 40,000 | 1e-3 | 0 | | 49 | 30.55 |
| full-mc → `models/full` | 12×64 | 8,000 | 2e-4 | 1.0 | full | 13 | 30.48 |
| tiny-mc → `models/tiny` | 6×24 | 8,000 | 3e-4 | 1.0 | tiny | 10 | 29.79 |

(`full` and `tiny` ran concurrently on the one GPU.) `configs/smoke.json` is
the same pipeline on 20 images and a toy network, for checking that
everything still runs.

### Tweaking

Every training knob is a flag of `unditherer.train` and a key of an
experiment in the config (`python -m unditherer.train --help`). The ones
that mattered:

- `--hue-p 0.5` — rotate the hue of half the crops through the full circle.
  Without it a corpus of browns never visits the palette's greens and blues.
- `--sat-min 0.6 --sat-max 2.2` — wide saturation jitter; TA's water and lava
  out-saturate any photo.
- `--mean-weight 1.0` — the block-mean consistency term on dithered samples
  (error diffusion preserves the local mean, so the output must too). The
  fine-tune that ships.
- `--vivid-p 0.4` — the "push saturation toward 255" augmentation (inside the
  hue branch) that was tried and made things worse; off by default, kept so
  the dead end is re-runnable (`configs/shipped.json` lists it under
  `dead_ends`).
- `--dither-modes fs,fs,fs,nearest,ordered4,ordered8` — the quantiser mix
  drawn per sample; repeat a mode to weight it.
- `--init` vs `--resume` — fine-tune (weights only, fresh schedule) vs
  continue (optimiser and schedule position too).

Single steps by hand:

```bash
python -m unditherer.fetch_data --dry-run                 # count and size only
python -m unditherer.train --out .data/undither-train/runs/mine --depth 12 --ch 64 --steps 40000
python -m unditherer.train --out .data/undither-train/runs/mine-mc --init .data/undither-train/runs/mine/best.pt \
        --mean-weight 1.0 --lr 2e-4 --steps 8000
python -m unditherer.evaluate .data/undither-train/runs/mine-mc/best.pt full tiny --json /tmp/eval.json
python -m unditherer.infer --model .data/undither-train/runs/mine-mc/best.pt \
        --in research/notes/assets/undither/shots --out /tmp/bake
python -m unditherer.credits                              # regenerate ATTRIBUTION.md
```

Long runs: launch in the background and read `runs/<name>/log.json` for
progress; it is rewritten at every validation.

## Tests

```bash
.venv-undither/bin/python -m pytest unditherer/tests            # 85 tests, ~8 s
.venv-undither/bin/python -m pytest unditherer/tests -m "not slow"
```

Covered: the palette and pair synthesis (every dither mode, seed
reproducibility, the augmentation knobs), every classical stage and preset
(PSNR gain on a synthetic dithered image, overrides, wrap borders, deband,
QA), the network (shipped parameter counts, identity at initialisation),
inference (ONNX metadata, tiled = whole-image, wrap padding, torch = ONNX,
export round-trip, `models.json` matches the files), the CLI (every preset and
flag, metric aliases, multiple inputs, WebP, colour-key alpha, reports, exit
codes, the learned presets through onnxruntime, the module entry point), the
pipeline (config resolution, flag mapping, dependency order, dry run, and an
end-to-end toy run that trains, fine-tunes, evaluates, exports and reports on
a synthetic corpus, CPU only), the fetcher (shipped manifest integrity, manifest
mode end to end against a fake network, verify, dry run), and the credits
generator.

## Licences

The corpus is CC0 1.0 throughout (ambientCG, Poly Haven, Screaming Brain
Studios via OpenGameArt); `CREDITS.md` has the verified terms, links and
names, `LICENSES/CC0-1.0.txt` the legal text. The game's palette and the
fifteen screenshots belong to the game's rights holders and are used only for
quantisation and evaluation.

## Relation to the rest of the repository

`tools/undither/restore.py` and `prep.py` are the original command-line
reference and the wiki's measurement script; `restore.py` now re-exports
`unditherer/classical.py`, so they and the WebGL viewer in
`research/notes/assets/undither/` share one implementation of every constant.
The wiki page `research/notes/undither.md` is the write-up.
