# Where a montage frame's time goes, and what a GPU would buy

Run these before optimising anything here. The first answer they gave was not
the expected one: the renderer was spending most of its time **manufacturing
pixels and throwing them away**, and no amount of GPU would have made that
right — it would have made it fast.

Everything below is measured on a 57 s 4K cut of `promo/tacli-promo.json`
against six real clips, sampled every 2 s and weighted by the 60 frames each
sample stands for. "single-thread" is the whole film's compositing cost in one
process; the real render divides it across `-j` workers.

```bash
V=.venv-undither/bin/python      # from the main checkout; see CLAUDE.md
$V promo/bench/timeline.py  <shoot-dir> 3840 2160        # cost across the film
$V promo/bench/configs.py   <shoot-dir> 3840 2160        # CPU vs GPU, per config
$V promo/bench/configs.py   <shoot-dir> 3840 2160 --no-clamp
$V promo/bench/bench.py     <shoot-dir> 3840 2160 3,26,51  # + fidelity + jitter
$V promo/bench/fidelity.py  <shoot-dir> 3840 2160 /tmp/fid  # vs an older revision
```

`<shoot-dir>` holds `v2/<clip>.mp4` and a `cache2/` frame cache.

## 1. The cost is in the MIDDLE of the pull-back, not the wide shot

| | tile size | share of the film's cost |
|---|---|---|
| opening, 1 window | 2449 px | ~2 % |
| t = 26-34, 13-48 windows | 1073-486 px | **~35 %** |
| wide shot, 1000-1400 windows | 99-82 px | ~10 % |

A thousand small tiles are cheap. Two dozen large ones are not: cost follows
**output pixels x filter taps**, and PIL's reduction filter has support that
scales with the reduction ratio, so the taps grow as the ratio squared.

## 2. The supersample ladder was upscaling the footage 2-4x, then discarding it

`canonical_size` asked for `tile_px * SUPERSAMPLE`, uncapped. At a 1073 px tile
it asked for **4096** — from footage that is **1024** wide. The clip was upscaled
four times and resampled back down. It cannot add detail and it triples the cost
of the reduction that follows.

Capping the ladder at the source's own width (`native_w`) is **1.50x on the
whole film for nothing**, and it is provably free:

* the wide shot is **bit-identical** (PSNR 138 dB) — there the ladder already
  sits far below the footage width, which is where supersampling earns its keep;
* in the band that does change, **gradient energy moves by ±0.5 %** (no
  softening) and the **temporal second difference is unchanged**;
* the differences are sub-pixel edge phase on glyph outlines, 0.4-3 % of pixels.

End to end: the 369-frame 4K opening went **42.0 s -> 25.2 s**. At 720p with
`--supersample 1` it changes nothing (1m05 vs 1m07) — correct, because there the
ladder never reaches the footage width.

## 3. Every source fetch decoded a full PNG

The per-frame benches above all re-render the same `t`, so the frame cache is
always warm and **PNG decode never appears**. In a real sequential render it was
**26-56 % of the wall clock**, rising with the zoom-out:

| window | before | after | decode share |
|---|---|---|---|
| t = 14, 4 windows | 2.7 s / 20 frames | 2.7 s | 26 % -> 28 % |
| t = 28, 24 windows | 19.4 s | 19.5 s | 38 % -> 39 % |
| t = 40, 140 windows | 24.0 s | 12.1 s | 43 % -> 13 % |
| t = 52, 1260 windows | 19.3 s | **6.1 s** | 56 % -> **0 %** |

The cause: the only source above the 160 px thumb ladder was the extracted PNG,
and at 4K the canonical sizes run **254 to 2554**, so the 160 ladder is never
reached. **840 of 840 source fetches decoded a full PNG** — 33-47 ms for a
2048x1536 frame, to produce a 318 px tile.

**An LRU cannot fix this.** Each (clip, phase) advances one frame per output
frame, so every access is a compulsory miss: at t = 52 it was 456 misses against
104 hits. The answer is a cheaper read, not a bigger cache.

`Clip.LADDERS = (160, 512, 1024)` are raw memmapped arrays built **straight from
the mp4** — one ffmpeg pass that scales and hands over raw frames, ~3 s per clip,
12 s for all six at 512. Building them by decoding the extracted PNGs instead
costs minutes and buys nothing. 5.5 GB for the 512 ladder against a 37 GB PNG
cache.

Verified the same way as the cap: **bit-identical** wherever the PNG path still
serves (widths above the ladder), and **PSNR 49-51 dB, sharpness -0.1 %, jitter
+0-1 %** where the ladder does. The residual is ffmpeg lanczos against PIL
bilinear on the reduction, and lanczos is the better filter of the two.

## 4. Equal frame counts are not equal work

With one segment per worker, the dearest 123-frame segment is **81 s against a
38 s average** — the wall floor is **2.12x the ideal**, and the workers holding
the opening finish in 5 s and then idle for the rest of the render.

`--chunks` (default 4) cuts the film into four segments per worker so the pool's
queue balances them. Measured: **56 segments of 31 frames, 2m42 -> 1m34.**

It is not free, and the cost is not what it looks like:

* **+8 % file size and +14 % CPU**, from the extra I-frames and the shorter GOPs.
* It is **not** task overhead: `Montage` construction is **48 ms**, so the 42
  extra constructions cost 2 s against the 201 s of extra CPU observed. Caching
  the Montage per worker was tried and changed nothing (1m34.2 against 1m33.5).
* Quality is unaffected: 55 seams average **1.149** against **1.177** elsewhere
  — below average, and the worst 10 frames are still only the t = 7.8-8.1 camera
  lurch, with no seam among them.

`--chunks 1` restores one segment per worker and the fewest seams.

## 5. End to end, 4K, `-j 14`, `-preset medium`

| | wall | CPU | master |
|---|---|---|---|
| before | 3m19.4 | 34m00 | 226.5 MB |
| + cap the ladder at the footage width | 2m49.9 | 29m06 | 227.5 MB |
| + memmapped source ladders | 2m41.7 | 22m44 | 227.6 MB |
| + balanced segments | **1m33.5** | 26m05 | 245.5 MB |

**2.13x wall, 1.30x CPU**, no GPU, and every step verified against jitter *and*
sharpness. The remaining wall time is dominated by the encoder, which overlaps:
`-preset ultrafast` saves 6m15 of CPU and 5 s of wall.

## 6. The GPU, measured

Same harness, so the rows are comparable. These predate the ladder and the
balancing, so read them as "what the GPU does to the compositing half", not as
end-to-end numbers:

| | single-thread | vs A |
|---|---|---|
| **A** CPU draw + CPU composite, ladder uncapped | 778 s | 1.00x |
| **B** the cap | 517 s | **1.50x** |
| **C** B + exact GPU compositor (built, verified) | 322 s | **2.42x** |
| **D** C + source resampling on the GPU (projection) | 109 s | 7.1x |

**D is a ceiling, not a pipeline**: it excludes PNG decode and host-to-device
upload (measured: 8.7 ms for 189 MB, amortisable with a GPU-resident frame
cache). Do not quote it as a result.

Per stage: the batched source resizes are **40-97x** (7.1 ms against 562 ms for
20 of 2048x1536 -> 2044x1534), the compositor 1.7-3.6x. Peak VRAM 2.4 GB at 4K.

**Nothing here is shipped.** The CPU work above took the render to 1m34 without a
CUDA dependency, and the stage the GPU would help most — the source resizes — is
the same stage the memmapped ladder already gutted.

### The trap: a fast GPU compositor that is quietly WRONG

`gpucomp.py` is kept because it is the mistake, not the answer. It prefilters
the source to the tile's scale and then bilinear-shifts it into place — two
resamples — and it is **12-20x** faster than the CPU. It also reported **lower
jitter at every zoom**, which read as a win.

It was blur. Gradient energy was **23-40 % below** the CPU path. Steadier and
softer are the same measurement; only sharpness tells them apart.

`exact.py` resamples **once**, reproducing PIL's own filter (a separable triangle
whose support scales with the ratio) with per-tile weights, so there is no phase
quantisation at all — the CPU path rounds the sub-pixel offset to a quarter pixel
to keep its cache sharable and this does not have to. Result: **PSNR 54-83 dB,
sharpness ±0.0 %, jitter identical** — and 1.7-3.6x instead of 12-20x. That is
the honest price of being right.

**So: never judge a resampler on jitter alone.** Measure gradient energy in the
same pass, or a blur will look like a fix.

## 7. What is next, in order

1. **Captions.** A full-frame card costs **95-102 ms per 4K frame** and 42 % of
   the film has one live: ~70 s of the film's single-thread cost, and a much
   larger share now that everything around it is faster.
2. **The 1024-2554 px band still decodes PNGs** — t = 14-34, where the hero-res
   clips ask above the 1024 ladder. A 2048 ladder would be 68 GB; storing the
   extracted frames as JPEG instead is the cheaper answer, and untested.
3. **GPU source resampling**, if the CUDA dependency is ever acceptable.
4. Chrome drawing is 6-45 ms per frame and is cacheable by
   (style, size, title, lod) across frames. Nobody has needed it yet.
