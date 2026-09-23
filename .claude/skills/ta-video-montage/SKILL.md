---
name: ta-video-montage
description: Script, prototype and render promo/demo montage videos of the tacli stack — a declarative montage file, a white-box wireframe pass to settle timing, then the same script rendered against real captured game footage, and encoded to deliverables. Use when asked to make a promo, trailer, demo reel or feature video, or to re-cut one for a new release.
---

# TA video montage

A montage here is a **file, not an afternoon**. `promo/*.json` describes the whole
film — stage, windows, camera, captions — and `tools/tamontage` renders it. Nothing
about the cut lives in a shell history or in someone's memory, so a new release is
re-shot by re-running two commands, not by rebuilding the edit.

**Fold new lessons back into this file after every montage session.**

Launching and driving the game is the `ta-drive` skill; observing a running instance
is `ta-capture`. This file is only about turning that footage into a film.

## The two backends, and why the wireframe exists

`tools/tamontage render <script> -o out.mp4` takes `--backend`:

| backend | fills a window with | costs | what it is for |
|---|---|---|---|
| `wire` (default) | procedurally drawn white boxes | ~70 ms/frame, no game | settling **timing**: beat lengths, camera path, caption copy |
| `clip --clips <dir>` | real footage, `<clip-id>.mp4` per id | a capture session | the deliverable |

The backend is a **render-time** choice, not a property of the script. The same file
white-boxes today and renders the real thing the moment the clips exist. Iterate the
edit against `wire` until the human signs off on the timing, and only then spend a
capture session — a wireframe pass is about a minute, a capture session is an evening.

**The wireframe is deliberately schematic** — outlines, a HUD frame and moving dots,
not a mock of our renderer. Nobody looking at a prototype render should be able to
mistake it for footage, and nobody reviewing the timing should be arguing about
terrain colour.

## Every window wears the same frame — the game included

`Window.content` draws the window decoration ONCE and renders the source into the
client rect, so the terminal does not turn into a bare rectangle when the command
runs: it keeps its frame, its title bar and its buttons, and so does every tile on
the desktop. The title bar switches at the flip (`bash` → `Total Annihilation —
tacli:front1`), which the cross-fade handles for free because both sides carry the
same chrome.

Sources (`Terminal`, `WireGame`, `Clip`) therefore render **client content only** —
none of them draws a border. Putting the frame in one place is also what made
`detail_ramp` possible, since there is now a single set of hairlines to fade.

Counter-intuitively this **reduced** jitter rather than adding to it (4K, mean
second difference): t=30 3.173 → 1.145, t=40 5.927 → 1.773, t=46.5 21.957 → 9.947.
The bare tiles had a hard 1 px outline of their own; one faded frame replaced it.

### The CLIENT rect is what has to be 4:3 — not the tile

The game fills the client rect, and the title bar takes 5.5 % of the window's
height before the client starts. So a 1024x768 tile leaves a **1022x724** client
(aspect 1.412) and `Clip.frame`'s `resize` stretched every frame of footage
**5.9 % wide** — on a promo for a renderer whose whole claim is pixel accuracy.

`stage.tile` is therefore **1024x813**, which leaves 1022x767 (1.3325, 0.06 % off
4:3). Measure it, do not derive it by hand:

```python
img = Image.new("RGB", (1024, h)); x0, y0, x1, y1 = draw_window_chrome(img, st, "", 1024.0)
```

Changing the tile moves the cell centres, so the camera's `cy` moves with it:
hero 1 is `tile_h/2`, the 2x2 block is `(tile_h + gap_y)/2 + tile_h/2`. Both are
in the script; nothing else needed changing.

### A 4:3 window in a 16:9 frame: solve for the height, accept the sides

A window shot that fills the frame's width crops its own top and bottom, because
the window is 1.26:1 and the frame is 1.78:1. The opening shipped that way and the
owner caught it: at `cols: 1.3` the visible world height was **795.6** against an
**813**-tall tile, so the window was 17 units taller than the frame and its top and
bottom borders were simply cut off. It stopped reading as a window.

Frame the height and let the sides fall where they land:

```
visible world height = cols * pitch_x * (out_h / out_w)      # = cols * 612 at 16:9
cols = tile_h / ((1 - 2*margin) * pitch_x * out_h / out_w)
     = 813 / (0.90 * 612) = 1.476                            # 5 % clear top and bottom
```

It is aspect-only, so a 1080p cut of a 4K master is framed identically. The sides
go to 18.1 % each and that is not a fault to fix — a 1.26:1 window **cannot** be
inset vertically and tight horizontally at once. Check the answer against the
beats that already work: the 2x2 block at `cols: 3.05` sits at
`1690 / (3.05 * 612)` = 90.5 % of the height, so 5 % puts beat 1 in the same
frame as beat 2 instead of one bleeding and one not.

**Measure the margin on the encoded frame, not the arithmetic.** The background
`[16,18,22]` comes back as `[15,16,21]` through yuv420 and the terminal body is
`[16,19,26]` — three levels off the desktop. A bounding box thresholded at 8 finds
"no window" and reports a clean frame; take the background from the frame's own
corner and threshold at 3.

## Fine detail must FADE, never switch

`detail_ramp(lod_w)` ramps hairline visibility over lod_w 32 → 105 instead of
testing a threshold. A hard cutoff is visible as a glitch: the borders were on at
t=46 and **gone at t=47** as lod_w crossed 70 — the owner saw exactly that, "the
white line shimmers and then all of a sudden disappears".

The decomposition that matters: **solid fills are safe at any size, only hairlines
shimmer.** The title bar, the side panel and the bottom strip are solid and stay
drawn however small the tile gets; the border, the buttons, the panel divider and
the terrain grid all fade out with the ramp.

## The terminal

`terminal_style` (per script, or `style` per window) picks from `TERMINAL_STYLES`:
`classic`, `soft`, `chromeless`, `phosphor`, `bash`, `hero`. **`soft` is the shipped
look** (chosen 2026-09-10): rounded corners, muted titlebar, blue prompt, thin bar
cursor, generous padding — carrying the round brightly-coloured window buttons
(`header: "dots"`). `bash` is the same idea with Chrome's flat minimise / maximise /
close glyphs instead, kept for comparison. Compare them with
`promo/prototype-cuts/terminal-styles.py`, which renders one full frame each plus a
sheet.

**What actually read as unpolished was not the chrome — it was the emptiness.** The
window is 4:3 and the first four styles use the top ~20 % of it, leaving a void. The
fix that mattered was typographic: set the command large and compose the block
against the window (`valign: center`), since the terminal is alone on screen at
near-full-size for the first seven seconds.

**Size the type to FIT, never by a fixed multiplier.** `fscale: "fit"` finds the
largest size at which the longest line still fits the padded width. A hard 1.75×
looked right for one command and ran `big-battle` off the right edge; command length
changes per scenario, so the multiplier cannot be a constant.

## The shape of a script

```
stage    tile size + desktop gap. Cell (col,row) sits at (col*pitch, row*pitch),
         pitch = tile + gap. Cells may be negative.
windows  the hero windows, placed by cell. Each opens as a terminal, types its
         command, prints output, then cross-fades into the game IN THE SAME RECT.
fill     the filler grid: a cell range, a clip pool, and a reveal policy.
         fill.text spells a word in the tiles of ONE clip (see below).
camera   keyframes of (t, cx, cy, cols).
captions (t0, t1, text, sub).
```

**`cols` is the zoom, and it is how you would say it out loud** — how many window
columns fit across the frame. `cols: 1.476` is one window inset 5 % top and bottom
(see the framing arithmetic above); `cols: 44` is the wide shot. Read the beat
sheet without rendering:

```bash
.venv-undither/bin/python tools/tamontage probe promo/tacli-promo.json
```

## Hard rules

1. **Gate the filler grid with `fill.reveal.not_before`.** The filler windows are
   always running, so without a gate a neighbour sits at the edge of frame one and
   beat 1 is no longer "a lone terminal on an empty desktop" — it is a terminal with
   half a game beside it, and the whole premise of the opening is gone. Beat 4 needs
   the same protection: **exactly four windows**, or "3 more terminals pop up" reads
   as "some of the noise resolved into terminals". Set `not_before` to the moment the
   film is allowed to stop being scripted.
2. **Reveal times come from the camera, never from hand-authored open times.**
   `_first_visible()` scans the camera path and opens each filler cell `lead` seconds
   before the viewport reaches it, so nothing pops on screen — and **retiming the
   pull-back retimes 1600 windows for free**. Hand-authored open times are how a
   montage becomes unmaintainable at the first re-cut.
   The one deliberate exception is the first ring at `not_before`, spread over
   `wave` seconds: the desktop visibly filling up is a shot, not a glitch.
3. **Zoom interpolates in log space.** A pull-back from 3 to 44 columns interpolated
   linearly spends most of its running time near 44 and reads as braking hard at the
   end. In log space equal times cover equal *ratios*, which is what a steady zoom
   looks like. `Camera.at()` already does this; do not "fix" it.
4. **Tile detail keys on APPARENT size: the displayed width normalised to a
   1920-wide frame** (`lod_w = iw * 1920 / out_w`), never the raw pixel width and
   never the buffer it is drawn into. In absolute pixels the same shot decides
   differently at different output sizes — a 4K wide shot (88 px tiles) crosses a
   70 px threshold that the identical 1080p shot (44 px) does not, re-enabling every
   1 px border and grid line the threshold exists to remove. That alone was **23.754
   → 3.468** of wide-shot jitter at 4K.
   In the wide shot a tile is ~45 px, and a 1 px white border on 45 px is 4 % of the
   tile: the grid stops reading as a wall of running games and starts reading as
   woven fabric. Below 70 displayed px the outline and internal grid come off and
   the desktop gap does the separating. Since sources are now drawn into a canonical
   buffer ~3x larger (see *Stability*), `disp_w` is passed down explicitly — keying
   the test on the buffer width silently re-enables every 1 px border in the wide
   shot, which is exactly the shimmer the canonical buffer exists to remove. Judge
   this on a wide-shot still, never on a close one.
5. **Caches are per frame: sources by (id, frame, canonical size, displayed width),
   scaled tiles by output size and quarter-pixel offset.** Tile size changes every
   frame during a zoom, so a cache living across frames only thrashes. The
   quarter-pixel quantisation on the tile cache is what keeps the sub-pixel path
   from costing 1600 resizes per wide-shot frame; a quarter pixel is far below
   anything visible, so the sharing is free.
6. **Render cost is dominated by big tiles, not by many tiles.** The 1600-tile wide
   shot and the single-terminal opening cost about the same. Do not optimise the grid
   before measuring.
7. **Treat every encode as unverified until it decodes end to end.** `ffmpeg -v error
   -i out.mp4 -f null -` must print nothing. Check the duration and frame count too.
8. **Importing `tools/tamontage` from a script needs `SourceFileLoader`** — it has no
   `.py` extension, so `spec_from_file_location` returns a spec with no loader and
   fails with a bare `'NoneType' object has no attribute 'loader'`.
9. **`fc-match` NEVER FAILS, so a fallback ladder below it is dead code.** Asked
   for a font the machine does not have it returns its best guess and reports
   success. "JetBrains Mono" is not installed on the reference setup, fc-match
   answered **NotoSans-Regular**, and the terminal — the thing on screen alone for
   the first seven seconds of the film — was typeset in a **proportional** face
   for its whole life, while every width calculation assumed a monospace one.
   `_fc_match` now compares the family it got against the family that was asked
   for and treats a mismatch as a miss (generic aliases — `monospace`,
   `sans-serif` — are exempt: whatever fontconfig picks for them IS the answer).
   Fixing it also changes every face the ladder resolves, so name the face you
   actually want: `Noto Sans` is listed explicitly under `sans` because that is
   the face every cut has been judged in.
10. **`ImageDraw` DISCARDS the alpha channel on an RGB image.** `fill=(255,255,255,8)`
   does not paint 3 % white, it paints **solid white**, silently. The wireframe's
   "faint" grid was full-brightness for its whole life — which is what made the tiled
   wall read as woven fabric and drove much of its shimmer. Blend the colour by hand
   (`c + (255-c) * f`) or draw on an RGBA layer and composite.
11. **Extracting clip frames: `-r <fps>` alone, never with `-vsync`/`-fps_mode`.**
   ffmpeg refuses the pair — *"One of -r/-fpsmax was specified together a non-CFR
   -vsync/-fps_mode. This is contradictory."* — and CFR is what the renderer needs
   anyway, because `Clip.frame()` maps time to a frame index by multiplication.
   A variable-rate extraction silently desynchronises every tile.
12. **The thumb ladder is cached to `.npy`, and that cache is the difference between
   a 4-minute render and a 25-minute one.** Every clip frame is decoded and
   downscaled at startup — 600 decodes per clip on a 20 s capture, paid on *every*
   run. Measured on six synthetic clips: startup fell from ~60 s to ~2 s once
   `thumbs-<W>.npy` existed. Delete the cache directory after re-capturing, or the
   render uses the old footage without saying so.

## Stability: why the picture shimmered, and the knob that fixes it

The first cut shimmered — terminals and tiles crawling even where nothing moved.
It was **the harness, not the content**, and the proof is one experiment: freeze
every source's animation so only the camera moves, and the jitter is unchanged
(108.658 frozen vs 108.732 live — **99.9 %** of it was the renderer).

The cause: sources were drawn *at the destination size*. The camera moves
continuously, so a tile's pixel width creeps past a whole number every few frames,
and every feature inside was positioned as `int(w * k)` — one pixel of `w` moved
the grid, the HUD, the text and the dots by differing amounts. Measured: `iw`
stepped 1383→1390 over one second while the origin rounded independently.

The fix is two-part, and both halves matter:

* **Draw at a canonical size off a ladder, then scale to the destination.** Content
  is then stable in its own space and only *resampled* as the camera moves.
* **Place sub-pixel.** Take `ceil`/`floor` of the destination span and map it back
  into source coordinates for `resize(..., box=...)`, instead of rounding origin
  and size independently.

### Measuring it: use the temporal SECOND difference

**Counting changed pixels is the wrong metric and will send you backwards.** By that
measure the fix looked like a 50 % regression (32.5 % → 48.3 % of pixels changed) —
because snapping holds still and then jumps, while correct sub-pixel motion changes
many pixels a little, every frame. Shimmer is *discontinuity*, so measure
`|2f(n) − f(n−1) − f(n+1)|`: smooth motion cancels, popping does not. (Same idea as
`ta-capture`'s "differs from both neighbours while the neighbours agree".)

| wide shot, mean 2nd difference | jitter | cost/frame @1080p |
|---|---|---|
| drawn at destination size (old) | 108.7 | 39 ms |
| `--supersample 2`, bilinear | 43.2 | 434 ms |
| **`--supersample 3`, bilinear (default)** | **30.7** | **628 ms** |
| `--supersample 3`, lanczos | 36.3 | 931 ms |

**LANCZOS is worse than BILINEAR here, and dearer** — its ringing on a hard window
border is itself a temporal artifact. Do not "upgrade" the filter. **4× is identical
to 3×** because the ladder quantises both to the same canonical size, so 3× is the
knee. `--supersample 1` restores the old fast path for drafts where only pacing is
being judged.

Some residual shimmer is inherent: a wall of 45 px tiles of high-contrast synthetic
art with 1 px borders and a regular grid is close to a worst case for minification.
Real footage is organic and should alias far less — **but that is a prediction, not
a measurement**, and it gets checked against the first real capture.

## Never move the camera while text is on screen

Measured on the prompt region during typing, temporal second difference:

| camera | jitter |
|---|---|
| the old "subtle" slow push (tile grew 1355 → 1373 px over the beat) | 7.453 |
| **frozen** | **0.000** |

A zoom of any size continuously resamples the tile, so every glyph stem crawls.
Pixel-identical is achievable and is what it should be: **freeze the camera for the
whole type / hold / enter beat**, and settle the next framing *before* the next
terminal starts typing. Do not put a "gentle" push back in — it reads as a defect,
not as production value.

**A hold must be EXACTLY constant in `cols`, and two keyframes that look like a hold
may not be one.** This was shipped and missed: the pair `{t: 10.5, cols: 3.05}` and
`{t: 25.0, cols: 2.95}` reads as a hold in the file and is a 14-second slow zoom in
the picture, creeping the tiles 593.8 → 599.3 px — right across the beat where
terminals 2-4 type. Check the *rendered* tile width across a hold, not the
keyframes' intent.

The same rule sets the beat structure: type, then **hold** long enough to read the
command (2 s, at the owner's instruction), then Enter, then output, then flip.

## Captions wrap, and shrink if wrapping is not enough

`_fit_text` wraps to `width` (default 0.84 of the frame) and steps the size down
until the widest line fits in `max_lines`. This is not cosmetic: "A test harness that
happens to look like a war." typeset at card size is wider than 1920 px and simply
ran off both edges of the frame. Explicit `\n` is honoured.

## A word on the wall: `fill.text`

The wide shot is a wall of ~1600 tiles, and tiles of one dark clip on a wall of
light ones read as ink. `fill.text` spells a word that way:

```json
"text": { "string": "IMPURE", "clip": "big-battle", "origin": [-13, 5], "gap": 1 }
```

`TEXT_FONT` in `tools/tamontage` is a 7-row cap font with variable widths (I is 3,
M/N/T/V/W/X/Y are 5, the rest 4); `text_cells()` turns the string into a set of
`(col, row)` cells from `origin` (the block's top-left cell) with `gap` empty
columns between glyphs. In `_build`, a filler cell on a stroke gets `text.clip`
and **every other filler cell gets one of the other clips** — the word's clip
appears nowhere else in the fill, because one stray dark tile in the field is a
smudge on the letters. The draw's variant is kept and only its clip swapped, so
the phases and reveal times of the rest of the wall are what they were.

Three things that were measured rather than assumed, in the order they bit:

1. **Which clip is the ink is a luma measurement, not a guess.** Mean luma over the
   cached 160 px ladder: big-battle (Town & Country, a brown city grid) **44**;
   air-war 70, last-stand 70, naval-push 74, ridge-assault 68, shore-raid **99**.
   One clip is dark and five are light, which is exactly the situation the trick
   needs. Two dark clips would have needed the darker one AND both out of the field.
2. **Centre the word on the camera and the title card lands on it.** The obvious
   placement — centred on the final camera target, 1.3 % off either way — put the
   card's plate (39-61 % of the frame height) over P-U-R for the last 5.5 s, with I-M
   and E poking out either side. So the word sits BELOW the card: rows 5..11 span
   64-89 % of the height at `cols` 40 and 63-86 % at 44. It enters from the bottom
   edge as the camera pulls back, is fully in frame from ~50 s, and the card then
   drops in above it with the URL line directly over the word.
3. **A hero window inside the block has to agree with the mask**, because heroes
   keep their scripted clips: on a stroke it must already play the word's clip, off
   a stroke it must not. `_build` prints a `WARNING` for any hero that disagrees.
   The centred placement needed a search for this (origin (-13,-2) put hero 0,
   big-battle, on the P's stem and the other three in the gap column); the placement
   under the card has no hero inside it and needs none. What remains is hero 0
   itself: big-battle at (0,0) is a **single dark tile at dead centre** from ~47 s
   until the card's plate covers it at 51.5 s. It is the window the film opened on
   and its clip is the transcript's, so it stays.

Check it on the draft path, not the master: `--t0 44 --t1 57 --width 1920 --height
1080 --supersample 1 --preset ultrafast` renders the whole tail in **3.4 s** wall
at `-j 14`, and frames at 50 s, 51.4 s (the last clean frame before the card) and
the final frame are the three that matter. The full 4K re-render is unavoidable
here: the assignment changes every filler tile the camera ever sees from 25 s on,
so there is no segment boundary to splice at.

## Speed: `-j`, and the three self-inflicted wounds before it

**A full 4K render of the 54 s cut takes 2m22s with `-j 16`.** Getting there was
mostly undoing redundant work, not clever optimisation — profile before reaching for
a GPU.

| | 4K wide-shot frame | jitter |
|---|---|---|
| drawn at destination size (the start) | — | 108.7 |
| after the supersample fix | 1683 ms | 30.7 |
| after the cache + ladder fixes | 621 ms | **8.45** |
| full 54 s cut, `-j 16` | **2m22s total** | |

The three wounds, all found by `cProfile`, none guessable:

1. **A continuous random `phase` per filler window made every tile a unique
   source**, so the per-frame cache never shared: 251 source draws and 252 resizes
   for ~234 visible tiles. `PHASES` quantises it. Keep
   `len(clips) * VARIANTS_PER_CLIP * PHASES` **well below** the on-screen tile count
   or the cache cannot do its job.
2. **A 2× ladder wasted two thirds of every draw.** A 211 px tile asked for 633 and
   got rounded up to 1024. Finer ~1.25× steps cut the cost *and* the jitter (30.7 →
   8.45): coarse steps mean the canonical size holds, then **doubles**, and every
   tile re-rasterises hard at that jump.
3. **`_game()` keyed on `variant`, splitting 6 clip files into 24 `Clip` objects**,
   each decoding and holding its own copy of every frame — which made the clip
   backend *slower* than the procedural one (1178 ms vs 621 ms). Variant only
   recolours a white box; it must not split a real clip.

Captions also composited the whole frame: converting 4K to RGBA and back cost ~93 ms
per frame regardless of caption size. Now only the dirty rect is composited (a full
card still dirties everything, and then it is the same work).

### Memory is the limit at 4K, and the failure is a HANG, not a crash

**A render of the real footage was killed by the OOM killer at 5-6 GB per worker,
and it did not look like a crash — it looked like a slow render.** Progress stopped
at "10/16 segments" with every worker idle, no message anywhere, for as long as you
let it sit. `multiprocessing.Pool` replaces a dead worker and silently loses the
task it was running, so the parent waits on a result that can never arrive.
`cmd_render` uses `concurrent.futures.ProcessPoolExecutor` now, which raises
`BrokenProcessPool`. The diagnostic that found it: sample `/proc/<pid>/stat`
utime+stime over ten seconds for every worker — all zero means wedged, not busy.

**Bound the frame cache in BYTES.** `FULL_LRU = 192` frames is 460 MB of 1024x768
footage and **1.8 GB of 2048x1536 footage, per clip, per worker** — the same
mistake as keying LOD on absolute pixels, because a count is not a budget when the
thing being counted changes size. `FULL_LRU_BYTES` (192 MB per clip) makes the six
clips fit in ~1.2 GB whatever they were shot at. The synthetic stand-ins hid this
completely: they were small and short, so the cache never filled.

**Do not materialise the thumb ladder.** It is `np.load(..., mmap_mode="r")`, and
the old code then copied every frame into the worker's heap with `np.asarray` —
~100 MB per clip per worker of pure duplication. `_ThumbView` indexes the mapped
array and builds the 160x120 image on demand, so the pages are shared through the
page cache.

After all three: **15.1 GB across 12 workers** for the 4K cut, against 5-6 GB per
worker before. `-j` is a memory knob as much as a speed one at 4K.

**Known race, not yet fixed:** every worker constructs its own `Montage`, so on a
COLD cache all of them try to extract the same clip at once, and `_extract`'s
`.done` stamp would let one worker `rmtree` a directory another is writing into.
It has not bitten (extraction happens to win before the others look), but do not
assume a cold-cache parallel render is safe. Warm the cache with a short
`--t0/--t1` render at `-j 1` first if it matters.

### `-j`: frames are independent

`--jobs` (default: half the cores) renders contiguous **segments** in parallel, each
worker encoding its own mp4, concatenated with `-c copy`. Workers encode their own
segments rather than shipping frames back, because a 4K frame is 24 MB and piping
1600 of them through IPC costs more than rendering them.

**Verify a parallel render at the seams**, since `-c copy` concat is exactly the kind
of thing that fails silently: count decoded frames (`-count_frames`), decode end to
end, and check the temporal second difference at multiples of the segment length.
Measured on the 4K cut: seams averaged **3.395** against **3.615** elsewhere — below
average, and no seam in the worst 20 frames.

### The ladder was upscaling the footage and throwing it away

**Profile before optimising, and profile the RIGHT frame.** The expensive part of
this film is not the 1400-tile wide shot — it is `t = 26-34`, where 13 to 48 tiles
are still 486-1073 px across. That band is ~35 % of the film's cost; the wide shot
is ~10 %. Cost follows *output pixels x filter taps*, and PIL's reduction filter
has support that scales with the ratio, so taps grow as the ratio **squared**. A
thousand small tiles are cheap; two dozen large ones are not.

And the ratio was inflated on purpose. `canonical_size` asked for
`tile_px * SUPERSAMPLE` with no cap, so a 1073 px tile asked for **4096** — from
footage **1024** wide. The clip was upscaled 4x and resampled straight back down:
it cannot add detail, and it triples the cost of the reduction that follows.

`canonical_size` now takes `native_w` and caps there (never below the displayed
size — the chrome is drawn at this size too). **1.50x on the whole film for
nothing**, and free by construction rather than by taste:

* the wide shot is **bit-identical**, PSNR 138 dB — there the ladder already sits
  far below the footage width, which is exactly where supersampling earns its keep;
* in the band that does change, **gradient energy moves by ±0.5 %** and the
  **temporal second difference is unchanged**;
* what differs is sub-pixel edge phase on glyph outlines: 0.4-3 % of pixels.

End to end the 369-frame 4K opening went **42.0 s -> 25.2 s**. At 720p with
`--supersample 1` it changes nothing — correct, because there the ladder never
reaches the footage width.

### Every source fetch was decoding a full PNG

**A per-frame bench cannot see this, and that is why it survived.** Every bench
here re-renders the same `t`, so the frame cache is always warm. In a real
*sequential* render, PNG decode was **26-56 % of the wall clock**, rising with
the zoom-out — 56 % in the wide shot.

The 160 px thumb ladder is never reached at 4K: the canonical sizes there run
254 to 2554, so **840 of 840 source fetches decoded a full PNG** — 33-47 ms for a
2048x1536 frame, to produce a 318 px tile.

**An LRU cannot fix it.** Each (clip, phase) advances one frame per output frame,
so every access is a compulsory miss: 456 misses against 104 hits at t = 52. The
answer is a cheaper read, not a bigger cache. `Clip.LADDERS = (160, 512, 1024)`
are raw memmapped arrays built **straight from the mp4** — one ffmpeg pass that
scales and hands over raw frames, ~3 s a clip. Building them by decoding the
extracted PNGs costs minutes and buys nothing. The wide shot went **19.3 s ->
6.1 s per 20 frames, decode 56 % -> 0 %**, bit-identical where the PNG path still
serves and PSNR 49-51 dB / sharpness -0.1 % / jitter +0-1 % where the ladder does.

### Equal frame counts are not equal work

One segment per worker looks balanced and is not: the dearest 123-frame segment
is **81 s against a 38 s average**, so the wall floor sits at **2.12x the ideal**
while the workers holding the opening finish in 5 s and idle. `--chunks` (default
4) cuts four segments per worker so the pool's queue balances them — **2m42 ->
1m34** on the 57 s 4K cut.

The price is **+8 % file size and +14 % CPU** from the extra I-frames and shorter
GOPs. It is *not* task overhead — `Montage` construction is 48 ms, so 42 extra
constructions cost 2 s against 201 s observed, and caching the Montage per worker
was tried and changed nothing. Quality is unaffected: 55 seams average **1.149**
against 1.177 elsewhere. `--chunks 1` restores the old behaviour.

### Where the render actually stands

4K, `-j 14`, `-preset medium`, on the reference setup:

| | wall | CPU | master |
|---|---|---|---|
| before | 3m19.4 | 34m00 | 226.5 MB |
| cap the ladder at the footage width | 2m49.9 | 29m06 | 227.5 MB |
| + memmapped source ladders | 2m41.7 | 22m44 | 227.6 MB |
| + balanced segments | **1m33.5** | 26m05 | 245.5 MB |

**2.13x wall, no GPU**, every step verified against jitter *and* sharpness.

### Steadier and softer are the same measurement

A GPU compositor prototype came out **12-20x** faster than PIL *and* reported
**lower jitter at every zoom**. It was wrong. It resampled twice — prefilter to the
tile scale, then bilinear-shift into place — and the "steadiness" was blur:
**gradient energy 23-40 % below** the CPU path.

**Never judge a resampler on the temporal second difference alone.** Blur lowers it.
Measure gradient energy in the same pass, or a softening bug looks like a fix.

Resampling **once**, reproducing PIL's own separable triangle filter with per-tile
weights, gives **PSNR 54-83 dB, sharpness ±0.0 %, jitter identical** — and 1.7-3.6x
instead of 12-20x. That is the honest price of being right, and it is what
`promo/bench/exact.py` does.

### What a GPU is actually worth here

Measured in one harness (`promo/bench/`, which has the full table and how to
re-run it). These rows predate the ladder and the balancing, so read them as what
a GPU does to the **compositing half**, not as end-to-end numbers:

| | single-thread cost of the film |
|---|---|
| ladder uncapped, all CPU | 778 s |
| **the cap (shipped)** | **517 s** |
| + exact GPU compositor (built, verified) | 322 s |
| + source resampling on the GPU (*projection*) | 109 s |

The last row is a **ceiling, not a pipeline** — it excludes PNG decode and upload.
Do not quote it as a result. The stage that would pay for the CUDA dependency is
the **source resizes** (40-97x batched: 7.1 ms against 562 ms for 20 of
2048x1536), not the compositor. Peak VRAM 2.4 GB at 4K.

**Nothing GPU is shipped**, and on this evidence it should not be first: the CPU
work above took the render to 1m34 with no CUDA dependency, and the stage a GPU
would help most — the source resizes — is the same stage the memmapped ladder
already gutted.

**Captions are the next real target**: a full-frame card costs **95-102 ms per 4K
frame** and 42 % of the film has one live. That was ~12 % of the cost before today
and is a much larger share now that everything around it is faster.

### Where the time actually goes: the encoder is FREE

Asked "is it encoding or rendering?", measure it — one flag answers it. Same 369
frames at 3840x2160, `-j 3`, warm cache:

| | wall |
|---|---|
| `-preset medium`, supersample 3 — what ships | **42.0 s** |
| `-preset ultrafast`, supersample 3 | 43.4 s |
| `-preset medium`, `--supersample 1` | **23.4 s** |
| `--supersample 1 --preset ultrafast` | 22.8 s |

**Making x264 as cheap as it goes changes nothing** (42.0 → 43.4 is noise, and so is
23.4 → 22.8). All of it is the compositor, and supersampling is ~45 % of that. The
practical consequence: **NVENC or any GPU encoder would buy exactly zero here**, and
`--preset` is not a speed knob — leave it at `medium` and take the quality.

### Draft fast, master once

Three independent levers, all measured on the real footage:

| | |
|---|---|
| `--t0/--t1` | render one beat. The opening, 0-12.3 s at 4K: 42 s instead of ~3.5 min |
| `--width/--height` | 1080p is a quarter of 4K's pixels. 0-11 s at 1080p: **10.5 s** |
| `--supersample 1` | the draft raster path: ~1.8x on top of the above |
| all three | **the whole 57 s film at 720p draft, `-j 14`: 1m07s** |

```bash
# check a framing or a timing change before paying for 4K
tools/tamontage render promo/tacli-promo.json -o /tmp/look.mp4 \
    --backend clip --clips <dir> --cache <dir> \
    --width 1920 --height 1080 --t0 0 --t1 11.0 -j 8
```

**A framing check at 1080p is EXACT for 4K** — `cols` resolves against `out_h/out_w`,
which is 16:9 either way, so the margin you measure on the draft is the margin that
ships. Only shimmer and fine-detail decisions need the real size, because those are
the ones keyed on apparent pixels.

### Do we need the GPU?

Not for encoding — see the table above; there is nothing there to speed up. For the
*compositor* it is feasible if it is ever needed: the reference setup has a CUDA GPU,
`torch` is already installed with CUDA support, and `libEGL_nvidia` is present, so a
`grid_sample` compositor needs no new dependency. Mipmapped texture sampling would
also beat supersampling for minification. But it is a rewrite of the draw path, not a
flag, and the CPU path was doing ~5x redundant work when it last looked slow — profile
first. Revisit only if 4K renders become frequent; at ~3.5 min each they are not.

## Honesty: what the film may and may not claim

A caption is a claim, and this repo's rule is that claims get checked against the
source before they ship. The montage script carries the copy; **this table carries the
evidence**, and a caption with no row here does not go in the film.

**Re-shoot every clip from the SAME scenario state.** The transcript in the film is
the tool's real output, so a scenario edited between takes makes the terminal print
something the command no longer prints — `big-battle`'s said "2 cleared" after
`clear_existing` became false, where a fresh run says "0 cleared". Shoot them all
after the last scenario change, then run `promo/transcripts.py`.

| caption | status | evidence |
|---|---|---|
| "One command." / "no menu, no map setup, no clicking" | VERIFIED | `tacli scenario load` is one command and is documented as "launch a game and put the situation in it" (`tools/tacli`, `scenario load` parser) |
| "silent · windowed · no intro movies" | VERIFIED | `tacli create` parser: `--sound` is "enable sound (default: silent)", `--intro` is "keep the intro movies" — both off unless asked |
| "A 1997 engine, mid-battle, in seconds." | VERIFIED | `ta-capture`: `tacli launch cap1 --res 1024x768` is "~2s to a window"; the scenario applier runs after |
| "Every fight is a file." / `scenarios/<name>.json` | VERIFIED | `scenarios/*.json`, 63 of them; `scenario validate` / `expand` / `apply` / `load` |
| "Instances share nothing." / "own game dir · own config · own window" | VERIFIED | each instance is `tagpu/instances/<name>/gamedir/` with its own config and window (`ta-capture`, `ta-drive`) |
| "Run one" / "or as many as the machine will hold" | **DELIBERATELY VAGUE** | nobody has measured how many instances the reference setup sustains, and the owner chose (2026-09-10) to keep the wording aspirational rather than spend a session measuring it. No count goes on screen unless it has been run |
| "Control units, spawn more on demand, send any command to a scenario as it runs." | VERIFIED (one caveat) | `tacli order` is "give units an order (no mouse, world coords)"; `scenario apply` is "mutate a live game", compiling entities into a **spawn table** applied from a detour inside the game tick (`research/notes/scenario-format.md`); ~18 verbs act on a live instance (`keys click order wheel eye gui arm peek weapons shot glshot log roster wait ui switches scenario apply`). **Caveat:** "any command" is loose — `create`/`launch`/`rm` are not sent *to* a running game. True in spirit, slightly overclaimed literally |

**The grid is a composite, and that is a disclosure, not a detail.** The wide shot
tiles a handful of real clips across 1600 cells. It is an ordinary montage technique
and it is fine — but never describe the film as "N instances running at once" unless
N instances were actually running. If the film wants that claim, shoot it: run as many
as the machine holds, count them, and say *that* number.

### The six promo scenarios

Written 2026-09-10, one per clip id, and all six validate. Every one of them
declares **`{"slot": 2, "controller": "off"}`**: the SKIRMISH screen comes up with
three slots filled, so without it a third player exists — its commander and its
buildings are on the map, and killing it off with `clear_existing` prints a
game-over message across the frame.

| scenario | map | entities | what the tile shows |
|---|---|---|---|
| `big-battle` | Town & Country | 980 | four columns converging on one point |
| `air-war` | Two Continents | 240 | 200 aircraft crossing, flak from below |
| `last-stand` | Two Continents | 427 | a fortified line against 400 attackers |
| `naval-push` | Anteer Strait | 85 | two fleets head-on mid-strait |
| `ridge-assault` | Two Continents | 149 | heavy armour, individually legible |
| `shore-raid` | Anteer Strait | 60 | ships shelling a shore battery |

Three rules they were built to, all of which matter *because* the clip gets tiled:

* **`camera.pin: true`, always.** A moving camera in a 48 px tile is noise, not motion.
* **`switches.noshake: true`.** Screen shake fights a pinned camera.
* **`"res": "1024x768"`** so the capture drops into `stage.tile` 1:1 (hard rule under
  *Capturing the clips*).

They are also written for **variety across tiles**, not just for looking good alone:
`ridge-assault` is deliberately smaller and wider-spaced so units stay individually
legible, and `shore-raid` is deliberately asymmetric — five tiles of "two lines meet"
repeated 1600 times reads as wallpaper.

**Placement without a live catalogue.** `scenario validate` checks the schema, but
without `--instance` it does **not** check unit names or map bounds. These six were
built only from unit types and coordinate neighbourhoods already proven by existing
scenarios, and their extents were then read back with `scenario expand` and checked
against those windows. On Anteer Strait that distinction is the whole game: water runs
east-west in a band around **y = 1680..2280** and the north shore is land from
**y ≈ 1620 up**, so a fleet grid one row too deep beaches its outer rank and those
ships never sail. Re-check with `expand` after any edit; do not trust `validate` alone.

### Open placeholders in `promo/tacli-promo.json`

* **CLOSED for window 0** (2026-09-10): its `output` is the real eleven-line
  transcript, captured from the shoot. Windows 1-3 still carry the invented format —
  the map names and unit counts in them are real (read back from `scenario expand`),
  but the surrounding layout is a guess at what `tacli scenario load` prints. Run
  each one once, capture the transcript, and match it; a promo that shows its own
  tool printing something it does not print is the kind of detail that gets noticed.
* Window 0's transcript says `launched 2048x1536` because that is the resolution the
  hero was shot at. Re-capture it if the shoot resolution changes — the line is real
  output, so it has to keep being real output.

## Capturing the clips

**Keep the terminal procedural and the game real.** The terminal's text is data in the
script, which means perfect typing timing, crisp glyphs at any zoom, and no dependency
on anyone's shell theme — but paste in the **real** transcript (rule above). Only the
game windows need footage.

**`promo/shoot.sh` is the shoot**, one clip per run — it loads the scenario, waits
for the fight to develop, grabs the window, stops the instance, and then verifies
what it got:

```bash
promo/shoot.sh big-battle <outdir> --instance front1 --settle 40 --len 60
promo/shoot.sh ridge-assault <outdir> --instance front5 --settle 40 --len 40 \
    --res 1024x768          # filler-only clips need no more than this
```

It writes `<clip>.mp4`, `<clip>.txt` (the real transcript) and `<clip>-settle.png`
(the frame the capture starts on, so a bad moment is caught before a 4K render
rather than after one). **Shoot a hero clip on the instance the film names** —
`front1` for `scenario load front1 big-battle` — or the transcript contradicts the
command printed above it; `promo/transcripts.py` refuses the mismatch.

Then fold the transcripts back into the script, which is what keeps the terminal
honest across a re-shoot:

```bash
promo/transcripts.py promo/tacli-promo.json <outdir>            # update
promo/transcripts.py promo/tacli-promo.json <outdir> --check    # CI-shaped
```

What the script encodes, all of it learned the hard way:

* **Ask `tacli` for the gamedir, never build the path.** Instances live in the main
  checkout, so `tagpu/instances/<name>/gamedir` resolved against a linked worktree
  is simply absent — which is how the settle screenshot silently wrote nothing on
  the first run of this script. `tacli ls --json` reports the real path.

Why a window grab and not a desktop grab:

* `-window_id` reads the window's own redirected pixmap, so the capture is that
  instance's frame **even when the window is covered or off-screen** — which is what
  makes a multi-instance shoot possible at all, and leaves the human's desktop alone.
* A region grab of a covered window records the covering window **silently**; the tell
  is a near-100 % duplicate-frame rate (`ta-capture` rule 5).
* `-draw_mouse 0`: the game draws its own cursor, so X's pointer is a second one.

### Capture resolution is set by the OUTPUT, and it is not one number

`stage.tile` is a **coordinate system**, not a pixel count — a clip of any size drops
into it as long as the **aspect matches** (4:3 here). What the capture resolution has
to satisfy is the largest the clip is ever displayed, and at 4K that is much bigger
than the tile:

| output | hero tile on screen | upscale from a 1024 capture | widest filler tile |
|---|---|---|---|
| 1080p | 1412 px | 1.38× | 41 px |
| **4K** | **2824 px** | **2.76×** | **82 px** |

So for a 4K deliverable the **four hero clips** must be captured at roughly
**2880×2160** (4:3), or the close shot is a 2.76× upscale and the 4K is spent on
nothing. The **filler pool can stay at 1024×768** — it is never shown above 82 px.
Mixing resolutions between hero and filler clips is fine and is the cheap play.

The engine **does** give it: `tacli scenario load front1 big-battle --res 2880x2160`
launched and ran (2026-09-10). The shoot used **2048×1536** instead, at the owner's
instruction (a 2160-tall window is too large for the reference setup's desktop),
which is a 1.38× upscale for a 4K hero — the same ratio 1024 gives at 1080p.

**But resolution is FIELD OF VIEW, not detail.** A windowed instance is 1:1 (game px
== window px, `ta-capture` rule 4), and the camera is pinned, so doubling the capture
resolution **doubles how much map is on screen** — it does not render the same
framing with more pixels. Measured on the `big-battle` capture: world = screen +
(3296, 3554), i.e. exactly 1 world unit per pixel, so 2048×1536 sees 1920×1536 world
units where 1024×768 sees 896×768.

Two consequences, and neither is obvious from the tile arithmetic above:

* **A hero clip and a filler clip of the same scenario are different shots.** The
  hero sees roughly four times the area. That is fine — arguably good, since the
  hero window is big on screen — but it is a composition decision, not a quality
  knob, and a scenario composed for a 1024×768 view has its action in the middle
  third of a 2048×1536 one.
* **The sharpness a bigger capture buys is real but bounded**: the units are the
  same size in pixels either way. What you gain is more battle, not a bigger battle.

Whatever you choose, keep the aspect at 4:3 and keep the window's CLIENT rect
matching it (above).

Shoot each clip **longer than the longest time it is on screen**, and shoot more
distinct clips than you think you need — `fill` gives each cell a random clip and a
random time offset, but six clips across 1600 tiles is still six clips, and the eye
finds the repeat.

### Survey the map before placing anything on water

**`promo/survey-map.py` answers "where is the water", and guessing does not.**
`scenario validate` checks the schema, not the shoreline, so a fleet placed one
row too far inland spawns ashore and sits there in formation for the whole clip —
which is what `naval-push` did on Anteer Strait with `core_fleet` at [3550, 1980],
and what `shore-raid` did with raiders whose eastern columns crossed the beach.

```bash
tools/tacli scenario load surveyA <an empty scenario on that map> --restart
promo/survey-map.py surveyA <outdir> --step 1400 --x0 1600 --x1 5200 \
    --y0 800 --y1 3600 --cell 200
```

It walks the camera, grabs the game window at each position, classifies every pixel, and prints a
world-coordinate grid. **Anteer Strait, measured 2026-09-11: the central island is
x 3000-4000, y 800-1700, and everything south of y = 1800 is open water.** The
note in `research/notes/` that said water runs at y 1680-2280 was describing the
island.

Three traps in doing this at all, each of which produced a confidently wrong map:

* **`tacli eye X Y` sets the EYE, not the centre** — the world coordinate at the
  left edge of the game area and the top of the view. Treating it as the centre
  shifts every sample half a screen and smears the land into the wrong cells.
  `tacli roster`'s own `eye` is one panel width smaller (world at screen x=0):
  `eye 4200 4200` reads back as `(4072, 4210)`.
* **Survey with `clear_existing: false`.** An empty scenario has no units alive,
  TA declares the game over instantly and drops to the 640x480 shell, and every
  grab is then a picture of the menu — reported as "0 % water" at every
  position. The script refuses a grab that is not the instance's resolution.
* **Survey with as few units as possible.** Units, wrecks and explosions are not
  blue, so they classify as land: the first attempt at reading the shoreline out
  of a battle screenshot mapped the fleet, and put the island's southern edge 500
  units too far south.

### A pinned camera changes what a scenario has to be

Every promo scenario pins the camera, so **the fight has to come to the frame and
stay in it** for the whole clip — 40 to 50 seconds, not the few seconds a battle
takes to resolve. Two failures of this, both only visible in the footage:

* **`air-war` was over before the capture started.** Two air forces ordered at each
  other across an empty view cross once and it is finished: 30 s after the load the
  frame held a landscape, some turrets and wrecks, and the take measured **90 %
  duplicate frames, ~6 unique fps**. Aircraft are fast and the camera cannot follow
  them. It is now built as 200 aircraft over two flak-defended bases 600 units
  apart, both inside the frame — targets they keep circling and re-attacking, and
  flak that keeps firing.
* **Ground battles burn down.** `big-battle` goes 980 → 934 units in the first
  minute and 790 in the second. Too early and the columns are still marching; too
  late and it is a mopping-up. `--settle` is the knob, and the settle screenshot is
  how you judge it without spending the take.

**The duplicate-frame rate is the metric that catches a dead scene** — it is the one
check that worked unassisted here. Anything under ~25 unique fps on a battle clip
means the fight is not in frame.

### Turn the fog off, or the tile is mostly grey

**`"los": "permanent"` and `"mapping": true` belong in every scenario shot for a
film.** Without them TA greys out every part of the map not currently inside a
unit's sight radius, and since the camera is pinned on the fight, that is most of
the frame: the first `big-battle` take read as a battle in the corner of an empty
grey city, and the "empty" part was fog, not terrain. With permanent line of sight
the whole map stays in full colour and the shot is a battle filling the frame.

Both are `setup` keys now (`scenario load --los/--mapping` still override), because
this is part of the *situation*: a scenario shot for a film wants the map lit, one
written to measure scouting wants the fog. `permanent` also means the fog never
creeps back over ground already seen, which on a 50 s clip would otherwise be a
slow grey wash across the tile.

### The GAME ends mid-take, and that is what truncates a clip

**When the last unit of a player dies TA declares the game over and returns to its
640x480 shell.** The window shrinks under the grab, `XGetImage` starts refusing
with BadMatch, and the take stops there — ffmpeg still exits 0. The tell that this
is what happened, rather than anything on the desktop, is that it is
**reproducible to the frame**: `last-stand` gave exactly 2897 of 3600 on three
consecutive attempts, `naval-push` exactly 3204. A flaky desktop does not do that.

**Every promo scenario therefore sets `clear_existing: false`.** The two starting
commanders stay alive at the map's start positions — far outside every pinned
camera — so no player is ever unitless however the battle goes. It also removes
the "Arm vermin have been exterminated" banner that clearing them printed across
the top of the frame at load.

Check each scenario's commanders really are out of frame (`tacli roster` gives
their world positions); on Anteer Strait they sit at (3488, 976) and (2477, 7344),
and on Town & Country at (4336, 304) and (640, 5561).

### A grab that ends early still exits 0 — count the frames

Four of six clips in the second shoot came back short and ffmpeg reported success
every time: **11 frames of 3600** for one, 2198 for another, 1341 for a third, and
one that refused to start with *"Capture area 1024x768 at position 0.0 outside the
screen size 640x480"*. The short ones die with a BadMatch on `ShmGetImage`, then on
`XGetImage`, and the demuxer gives up with "Permission denied".

That was the *primary* cause. A second, independent one made it worse:
**`tacli ls --json` could hand back the wrong window.** `Instance.window()`
searched by title, and a candidate whose pid could not be read was kept rather than
rejected — so a lingering, title-matching, pid-less window belonging to the instance
stopped seconds earlier could win. 640x480 is the giveaway: that is TA's **shell**
size, not any game resolution. It now ranks candidates by evidence (exact size +
confirmed pid beats either alone) rather than taking the first one found. Ranking,
not rejecting: some wine windows carry no `_NET_WM_PID` at all.

Two defences, both cheap, and the shoot script has both:

* **Assert the window is the size you are about to grab** before spending the take.
* **Check the frame count afterwards** — `nb_read_frames` against `fps * seconds` —
  and re-take if it is short. A duplicate-frame rate alone does NOT catch this: an
  11-frame clip scored 80 % duplicates, under the 85 % "wrong window" threshold, and
  was reported as ok.

### What the first real shoot measured (2026-09-10)

The `big-battle` hero clip, `--res 2048x1536 --maxfps 60`, grabbed by window id:

* **The game presents ~34 unique frames/s**, not 60: a 60 fps grab came back
  **43.3 % duplicate** frames (1500 frames, 25.0 s, 168 MB at `-crf 15`). Grab at
  60 and let the montage extract 30 — do not grab at 30 and hope the phase lines up.
* **Shoot the clip longer than the beat needs.** Hero 1 is on screen from `t_game`
  to the end of the film — **50 s**, not the 25 s captured here. `Clip.frame` wraps
  with a modulo, so a short clip silently loops, and a loop is visible on a window
  that big.
* **The battle burns down.** 980 units → 934 by the time the first shot was taken
  and 790 a minute later. Capture the window you want: too early and the columns
  are still marching, too late and it is a mopping-up.
* **`clear_existing: true` kills both commanders, which is a player being
  eliminated**, so the game prints "Arm vermin have been exterminated" / "Core
  forces have gone to a better place" across the top of the frame at load. They
  fade on their own — do not start rolling until they have.
* Per-player unit cap is **500**, full stop (`research/notes/scenario-format.md`,
  *UnitLimit is clamped to 500*). `big-battle` was written for 1200 and is now 980
  (245 per group, 490 per player, leaving room for the commander).

### The terminal shows a REAL transcript now

`promo/tacli-promo.json` window 0 carries the eleven lines `tacli scenario load`
actually printed. Two things had to change to make real output survivable:

* **`fscale: "fit"` must fit BOTH axes, and measure every line.** It sized for
  width only and picked the widest line by *character count* — which is not the
  widest line in pixels, and says nothing about whether twelve lines still fit down
  the window. The transcript ran off the right edge and lost its last line.
* **`out_fscale` sizes the output relative to the command** (0.62 on `soft`). With
  one shared size the command — the shot — is set to suit the longest line of a
  transcript nobody is meant to read, and the typing beat becomes a small line in a
  large empty window. Drop the key to get one true terminal size back.
* The printed lines are also **paced to the flip**: `Terminal` takes `t_done` and
  spreads the reveal over the time the cut gives it, instead of a fixed 0.22 s per
  line that leaves an eleven-line block still printing when the window flips.

## Encoding the deliverables

Render is CRF 18 by default, which is the master. Then:

* **Share cut** — try `-crf 25 -preset slow` at 1080p first and check the size. Grid
  content is high-frequency and compresses badly; the wireframe master is 43 MB for
  54 s and the CRF 25 cut is 24 MB. If a hard cap binds (GitHub attachments are
  10 MB on the free plan), **downscale to 720p before starving the bitrate** — fewer
  pixels to quantise beats more pixels encoded worse.
* Judge a candidate on a **wide-shot frame**, not on the number: that is where the
  cut either holds up or turns to mush.
* **Sending a cut to the owner caps at 30 MiB.** That is what fixes the share cut at
  CRF 25 rather than 23 — the 57 s 4K master at CRF 23/1080p is 34.3 MB and is simply
  refused. CRF 25 is 25.7 MB. Check the size before the upload, not after it fails.
* Verify the decode (hard rule 7) before sending anything.

### Re-render a beat, not the film: cut at a SEGMENT BOUNDARY

A note on framing or timing usually touches one beat. Re-rendering 1710 frames to
change 369 of them is waste, and it also re-rolls the encode of every frame the
owner has already approved. Splice instead — and the splice point is not free:

**The master's only keyframes are the parallel render's segment boundaries.** Each
worker encodes its own segment as a single GOP (123 frames at `-j 14`, well under
x264's 250-frame keyint), so `ffprobe -skip_frame nokey` on the 57 s cut returns
exactly 14 times: f0, f123, f246, f369, … A `-c copy` cut is only possible there.
So round the end of the changed range UP to the next boundary:

```bash
# the change ends at 10.5 s; the next boundary is f369 = 12.3 s
tools/tamontage render <script> -o head.mp4 --t0 0 --t1 12.3 -j 3 ...   # 42 s
ffmpeg -ss 12.3 -i master.mp4 -map 0:v -c copy tail.mp4
printf "file '%s'\nfile '%s'\n" $PWD/head.mp4 $PWD/tail.mp4 > splice.txt
ffmpeg -f concat -safe 0 -i splice.txt -c copy -movflags +faststart silent.mp4
ffmpeg -i silent.mp4 -i master.mp4 -map 0:v -map 1:a -c copy out.mp4   # audio unchanged
```

Choose `-j` so the head's own segments land on the SAME boundaries: 369 frames at
`-j 3` is 123 each, so the seams at f123 and f246 stay where they were and the
splice at f369 reuses a seam that already existed. **The cut adds no new seam.**

Two traps:

* **Do not splice through the raw elementary stream.** Converting both files to
  Annex-B with ffmpeg's mp4-to-Annex-B bitstream filter, concatenating them and
  remuxing at `-r 30` looks cleaner and silently **drops the first two frames**
  (1708, not 1710) on the initial negative DTS that the mp4 edit list was there to
  absorb. The concat demuxer handles it; the elementary-stream route does not.
* The extracted tail reports `start_time=0.066016` from that same edit list, and
  `-avoid_negative_ts make_zero` does not clear it. It does not matter — the concat
  demuxer re-bases each input by accumulated duration and ignores it.

**Take the audio from the old master with `-c copy`.** The film's length did not
change, so the AAC is still correct, and copying it is byte-exact — no re-mux of the
mp3, no chance of drifting `start`.

Verify three things, not one: **1710 frames and 57.000 s**; the keyframes back in
their old places; and `mean |delta|` against the previous master **~0 after the cut
point** (0.001 measured) and non-zero before it. The last one is what proves you
changed the beat you meant to and nothing else.

## The soundtrack: score it AFTER the render

`--audio <file>` lays a track over a finished cut. The video stream is **copied, not
re-encoded**: measured at **0.5 s** for the 57 s / 227 MB 4K master. That is the reason
the track is chosen last — auditioning a candidate costs seconds, so there is no
reason to commit to one before the picture is locked.

The script carries the timing and the credit; the **path is a command-line argument
on purpose**, because a path names someone's home directory and the track is licensed
for use *inside* a work rather than for redistribution. It never enters the
repository. `promo/MUSIC.md` holds the full metadata and the dispute answer.

### Align the music's hit to the film's hit, never the two files' ends

This shipped wrong once. The mp3 is 40.18 s long, but **~5 s of it is digital
silence** — the music peaks at 31.5 s and resolves by ~35 s, which is why the source
page calls it 0:36. Setting `start = 57.0 - 40.18 = 16.82` from the *file length* put
the peak at 48 s and left the climax — the wide shot and the title card — playing over
dead air. The rule is `start = duration_of_film - music_ends`, and `music_ends` is
**measured**, not read off the file:

**`astats` is not the tool.** `-af astats=metadata=1:reset=1` writes at ffmpeg's
*info* level, so `-v error` swallows it, and the `ametadata=print` variant needs an
explicit `file=-`. Decode to mono PCM and do the arithmetic instead — unambiguous,
and it works the same on the mp3 and on the muxed cut:

```bash
ffmpeg -v error -i cut.mp4 -map 0:a -f s16le -ac 1 -ar 8000 - \
| python3 -c "
import sys, numpy as np
a = np.frombuffer(sys.stdin.buffer.read(), np.int16).astype(float)
sec = a[:len(a)//8000*8000].reshape(-1, 8000)
db = 20*np.log10(np.maximum(np.sqrt((sec**2).mean(axis=1)), 1)/32768)
for i, v in enumerate(db): print(f'{i:3d}s {v:7.1f} dB', '#'*int(max(0, v+60)/2))
"
```

`start` 22.0 lands the peak at 53.5 s on the title card. A long silent opening is
usually **arithmetic, not taste**: 35 s of music cannot cover 57 s of film *and*
finish at the end, and the climax is at the end.

**Verify by measuring the muxed output's RMS per second, not by listening alone** —
the failure mode is "silent where it should build", which a quick scrub hides.

### CC BY is a real obligation and the renderer will not let you forget it

`_mux_audio` prints the required credit **every time it muxes**. Put it where the
film is posted — the video description and the README that embeds it. A credit that
exists only in `MUSIC.md` does not discharge the licence.

A content-ID match on a Creative Commons library track is an **expected event**, not
a sign anything is wrong. Keep the ISRC and the download date; that plus the licence
is the dispute answer.

## The typing: real recordings, sliced, on the picture's own timing

`audio.keyboard` names which terminals are heard; `--keys <recording>` supplies a
take of someone typing, which is **cut into individual one-shots at render time**
and one placed on each keystroke.

```json
"keyboard": { "windows": [0], "gain_db": -5.0, "seed": 20260911, "pitch": 0.06 }
```

**Synthesised clicks were built first and rejected.** They cost nothing, carry no
licence and regenerate from a seed — and they did not sound good. Three voices
were auditioned and the owner's verdict was "doesn't sound very good". No
measurement would have caught that: the synthesis was *correct* (right envelope,
right spectrum, right variation) and still wrong. **When the deliverable is a
sound, the only gate is someone listening.** Build the audition early and cheaply.

Slicing a continuous take: detect onsets on the **high band** (a keystroke IS a
click; the low end is room and body that smears the attack), cut a window at
each, and drop the ones that are clipped or crowded by their neighbour. The
biggest, lowest-frequency one-shots are the space bar and the other long keys, so
they become the bank for space and Enter. A 36-59 s take yields 40-300 usable
one-shots. Placement varies pitch ±6 %, level ±25 %, pan, and never repeats a
click within four.

The guard on `--keys` only catches *silence*. Feed it the music by mistake and it
will happily find "keystrokes" in an orchestral track and render them.

### Snap each keystroke to the FRAME its glyph appears on

`Terminal._typed` shows character *i* once `int((t - t_type) * rate) >= i + 1`,
and it is only ever sampled at frame times — so the analytic instant is up to one
frame **early**. Snapping to the frame is both the correct sync *and* free,
natural-sounding jitter: 17 chars/s into 30 fps gives gaps of **33 and 67 ms**
instead of a metronomic 59, which is what stops it sounding like a machine gun.

## Stretching music for a video

The problem: 35 s of music, 57 s of film, the climax has to land at the end — so
the opening was silent to 22 s. The owner wanted the music at the first zoom-out
(7.47 s), with the typing kept dry. **The only way to start earlier without
moving the climax is to make the track longer at the front.** `audio.loop`
splices the track onto itself; `promo/loop-finder.py` finds where.

Five plans were built for this cue. Four were rejected by the owner's ear, and
every one of those rejections was *predicted* by a measurement — just not by the
measurement being used at the time. This section is the record, because the
next cue will be the same fight.

### What works

* **Level-match the join.** RMS over 0.5 s either side of the cut, on the
  *built* file; want within ±1 dB. This is the one metric that agreed with the
  ear every time. The plan that stepped −2.6 dB was heard at once.
* **Loop the flat body, never the crescendo.** Print the level per bar first.
  This intro rises +5 dB a bar; every backward jump inside it is a step down.
  The body sits within 2 dB for nine bars — that is where a loop lives.
* **Chroma finds harmonically matching bars.** 12 pitch classes per frame,
  mean-removed. Repeat period 2.07 s = one bar; downbeats at 0.30 + k·bar. A
  spectral envelope cannot do this: it scored every bar at ~0.997 against every
  other, because every bar is "orchestra".
* **Measure the bar length from the track — and from the harmony.** The stated
  115 bpm is 2.0870 s a bar; chroma says **2.0635–2.0717**. That 0.7 % is 37 ms
  over four bars, which is a stutter. The onset-envelope autocorrelation is
  *not* the tool: it has several peaks between 2.0 and 2.1 s and the search
  window decides which one wins (it returned 2.0198 and 2.0717 on the same file).
* **One join.** Each seam is a chance to be heard. Q, the plan that shipped, has
  exactly one: play to 21.48 s, jump back six bars to 9.018 s, play on.
* **A one-beat crossfade (500 ms), not 18 ms.** 18 ms is a click suppressor; it
  hides nothing musical, and turns every residual misalignment into a hiccup.
  On a through-composed cue a join is hidden by the *fade*, not by alignment.
* **Shift the whole cue rather than add a join.** Six bars reached 9.6 s; the
  brief was 7.47. Authoring `start` 2.1 s earlier costs a quiet tail from 53 s
  instead of 55 and the loudest second moving from 47 s to 45 s. A second join
  would have cost more.
* **`start` is authored against the unlooped track.** The loop only extends the
  front, so it pulls the start earlier by exactly its own length and nothing
  downstream moves. `tamontage` measures the built file's `added`, so the
  crossfade cannot drift it.
* **Verify on the output, then listen.** Level step at every join; loudest
  second unchanged; typing region still dry. Then the owner listens, because:

### What did not work, and why

* **Looping the intro (plan B).** Chroma join +0.894, the best in the track —
  and it stepped **−2.6, −2.6, −2.7, +4.0, +5.1 dB**, because the intro is a
  crescendo. Heard at 0:14. *Harmonic similarity says nothing about level.*
* **Level-matched, tempo from the metadata (plan D).** Joins within ±1 dB — and
  the 4-bar join landed **37 ms off the beat** because 115 ≠ 115.85 bpm. Heard
  at 0:24. The 1-bar join, with a quarter of the drift, was not heard.
* **Onset-envelope beat alignment.** Correlations of 0.31–0.39 — fitting
  noise. The "corrections" measured *worse* on the output (−60, −31, −56 ms).
* **Waveform cross-correlation to align joins.** Peaks of **0.19–0.33** between
  any two bars of this piece. That number is the real finding: **this is not
  loop music.** No two bars are waveform-alike, so no alignment metric locks and
  no short cut is inaudible. Stop aligning; lengthen the fade and cut fewer times.
* **Measuring sample-to-sample jumps to find "the stitch".** Found no clicks —
  correctly — while the owner was hearing a −2.6 dB level step. The wrong
  metric produces a confident wrong answer.
* **Equal-power crossfading identical material.** Two of plan D's joins were
  the same audio faded into itself; cos+sin peaks at 1.41. Merge self-joins out.
* **Overwriting the review page's file with a new cut at the same name.** It
  destroyed the A/B and produced "even the original seems changed". Every
  version gets its own name; nothing is overwritten.

### The procedure, next time

1. `promo/loop-finder.py <track> --from-max <before the climax's run-up>` —
   level profile, measured bar, and three level-matched single-jump candidates
   per span. The climax run-up is *as loud as the body*, so the level filter
   cannot see it; you cap it by ear.
2. Build the two or three plausible ones at 500 ms, mux onto the 1080p cut
   (`-c:v copy`, one second each), verify the level step on the output.
3. **Send them and wait.** On this material the ear is the only gate that
   settled anything, and it settled it every time.

## Reviewing a cut: `promo/review-server.py`

A 230 MB 4K file is not reviewable over chat. `promo/review-server.py <dir>` serves a
directory of cuts as a page with chapter buttons generated from the montage script.

Two things it must do, and both are load-bearing:

* **HTTP Range / 206 and a threaded server.** `<video>` seeking does not work without
  Range, and a single-threaded handler blocks the page while the video streams.
  `http.server`'s default does neither.
* **Bind to the Tailscale address only.** Never `tailscale serve` or `funnel` — those
  publish. The default `--bind tailscale` resolves the interface address and binds
  there.

It rebuilds its file list **at startup**, so a new cut dropped into the directory
needs a restart to appear. It labels by resolution when the files are renditions of
one cut and by name when they are not.

### Chrome does not load media in a BACKGROUND TAB, and it reports nothing

This cost a long detour: the video sat at `readyState 0`, `networkState 2`, **no
error, ever**, and every plausible cause — faststart, bitrate, CPU load from the
render — was wrong. The cause is that the tab was never foregrounded.
`document.visibilityState === "hidden"` suppresses the load, indefinitely and
silently.

So the page diagnoses itself rather than leaving the next person to re-derive it:

```js
function checkVisibility() {
  const stuck = v.readyState === 0 && !v.error;
  if (document.visibilityState === 'hidden') return;   // nothing to say yet
  hint.hidden = true;
  if (stuck) v.load();                                  // foregrounded: kick it
}
```

**Never conclude a served video is broken from a headless or background check.** The
only valid test is a foreground tab.

## Verifying a finished cut

Run all of these; each one has caught a real failure that the others missed.

| check | what it catches |
|---|---|
| `ffmpeg -i cut.mp4 -map 0:v -f null -` and again `-map 0:a` | a truncated or corrupt stream; **check the audio separately** |
| `ffprobe -count_frames` → exact frame count and duration | a `-c copy` concat that silently dropped frames |
| temporal 2nd difference at multiples of the segment length | a bad seam |
| the worst 10 frames by that metric, with their timestamps | *where* the jitter is — it is usually one camera move, not the seams |
| window bbox vs frame on a few opening frames | framing (see the 16:9 section) |
| per-second RMS of the muxed audio | music that starts or ends in the wrong place |
| `mean \|delta\|` against the previous master, per frame | after a splice: proves you changed only the beat you meant to |

The last one is the one people skip, and it is the only check that distinguishes
"I re-rendered a beat" from "I re-rendered the film and hoped it matched".

## Re-cutting for a new release

This is the case the whole design exists for:

1. Re-capture the clips into a fresh directory — same commands, new build.
2. `tamontage render promo/tacli-promo.json -o master.mp4 --backend clip --clips <new>`.
3. Re-check the claim table above: a caption that was true at v0.2 may not be at v0.3.
4. Re-encode the share cut, verify the decode.

The camera, the beats and the copy are untouched. If the *timing* needs to change,
change it in `wire` first — it is a minute per iteration there and an evening here.

## Status

`promo/tacli-promo.json` is the **57 s** tacli promo in the **Card treatment** —
full-frame typographic cards, chosen by the owner from five prototype cuts
(`promo/prototype-cuts/`). Shipped at 4K with a 1080p share cut. Scored. Public
since 2026-09-11 at https://youtu.be/MZCJUIns_6I on the Total Annihilation: Impure
channel: v5, which **spells IMPURE on the wall** in big-battle tiles under the title
card (`fill.text`, above), on top of v4's typing, one-join music and held banner.
**YouTube cannot replace a video's file**: a re-cut is a new upload and a new URL, and
the previous one goes private. v4 was public for a few hours at a URL that is now dead.

Two things the prototype changed in the base cut, both worth keeping:

* Beat 1's card moved to **after** the terminal flips to the game. Over the terminal
  it covered the command and the lines it printed — the one thing that shot exists
  to show.
* Captions wrap and shrink (above), because the card treatment typesets large.

What the finished film measures (2026-09-11):

* **The cut**: 1710 frames, 3840x2160, 57.000 s, aac stereo. Video and audio both
  decode clean. **238 MB** master, **25.7 MB** 1080p share cut at CRF 25.
* **`clip` pass: DONE.** All six clips shot 2026-09-11 — the four heroes at
  2048x1536 and the two filler-only ones at 1024x768, 27-35 unique fps each.
* **Both transcript placeholders are CLOSED.** All four hero terminals carry the
  real twelve-line transcript, captured after the last scenario change and checked
  with `promo/transcripts.py --check`.
* **Real footage aliases less than the wireframe did, as predicted.** That
  prediction is now a measurement.
* **The parallel concat is sound**: at `-j 14` the 13 segment seams average
  **1.369** against **1.177** elsewhere, and no seam appears in the worst 10 frames.
* **Scored.** "Evening Melodrama", Kevin MacLeod, CC BY 4.0 — `promo/MUSIC.md`.
  Verified by per-second RMS on the muxed cut: below -90 dB until **22 s**, loudest
  RMS second at **47 s** (-16.2 dB), loudest peak sample in second **52** — under the
  title card — and back below -30 dB by **55 s**.
* **The opening is framed 5 % clear top and bottom** (`cols: 1.476`). It shipped
  once at 1.3, bleeding off both edges; re-rendered as a 369-frame splice.

Open, and none of it blocking:

* **The owner has watched v1-v4 and chose each change by eye and ear**; the
  measurements above caught broken, the owner caught bad (the loop seams, the synth
  typing). v5 has been checked on frames only.
* **The t = 7.7-8.1 s camera lurch.** The worst frames of every render so far, and
  it is the easing at the 7.47 keyframe, not anything at 9 s.
* `air-war` leaves the outer thirds of its frame fairly empty, with the bases only
  600 units apart in a 1920-unit frame.
* `scenarios/ball10.json` asked for 625 units per player against the engine's hard
  cap of 500 and never created what it declared; since 2026-09-11 it asks for 500,
  the most the engine seats (the landing review found the old file refused by
  `validate` outright, which is not a fixture).
