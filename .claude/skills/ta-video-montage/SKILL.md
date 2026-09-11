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
camera   keyframes of (t, cx, cy, cols).
captions (t0, t1, text, sub).
```

**`cols` is the zoom, and it is how you would say it out loud** — how many window
columns fit across the frame. `cols: 1.3` is one window filling the height; `cols: 44`
is the wide shot. Read the beat sheet without rendering:

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
9. **`ImageDraw` DISCARDS the alpha channel on an RGB image.** `fill=(255,255,255,8)`
   does not paint 3 % white, it paints **solid white**, silently. The wireframe's
   "faint" grid was full-brightness for its whole life — which is what made the tiled
   wall read as woven fabric and drove much of its shimmer. Blend the colour by hand
   (`c + (255-c) * f`) or draw on an RGBA layer and composite.
10. **Extracting clip frames: `-r <fps>` alone, never with `-vsync`/`-fps_mode`.**
   ffmpeg refuses the pair — *"One of -r/-fpsmax was specified together a non-CFR
   -vsync/-fps_mode. This is contradictory."* — and CFR is what the renderer needs
   anyway, because `Clip.frame()` maps time to a frame index by multiplication.
   A variable-rate extraction silently desynchronises every tile.
11. **The thumb ladder is cached to `.npy`, and that cache is the difference between
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

### Do we need the GPU?

Not yet, and it would have been the wrong first move — the CPU path was doing ~5x
redundant work. It is feasible if it ever is needed: the reference setup has a
CUDA GPU, `torch` is already installed with CUDA support, and `libEGL_nvidia` is
present, so a `grid_sample` compositor needs no new dependency. Mipmapped texture sampling would
also beat supersampling for minification. Revisit only if 4K renders become frequent;
at 2m22s each they are not.

## Honesty: what the film may and may not claim

A caption is a claim, and this repo's rule is that claims get checked against the
source before they ship. The montage script carries the copy; **this table carries the
evidence**, and a caption with no row here does not go in the film.

| caption | status | evidence |
|---|---|---|
| "One command." / "no menu, no map setup, no clicking" | VERIFIED | `tacli scenario load` is one command and is documented as "launch a game and put the situation in it" (`tools/tacli`, `scenario load` parser) |
| "silent · windowed · no intro movies" | VERIFIED | `tacli create` parser: `--sound` is "enable sound (default: silent)", `--intro` is "keep the intro movies" — both off unless asked |
| "A 1997 engine, mid-battle, in seconds." | VERIFIED | `ta-capture`: `tacli launch cap1 --res 1024x768` is "~2s to a window"; the scenario applier runs after |
| "Every fight is a file." / `scenarios/<name>.json` | VERIFIED | `scenarios/*.json`, 63 of them; `scenario validate` / `expand` / `apply` / `load` |
| "Instances share nothing." / "own game dir · own config · own window" | VERIFIED | each instance is `tagpu/instances/<name>/gamedir/` with its own config and window (`ta-capture`, `ta-drive`) |
| "Run one." / "or as many as the machine will hold" | **DELIBERATELY VAGUE** | nobody has measured how many instances the reference setup sustains, and the owner chose (2026-09-10) to keep the wording aspirational rather than spend a session measuring it. No count goes on screen unless it has been run |
| "Control units, spawn more on demand, send any command to a scenario as it runs." | VERIFIED (one caveat) | `tacli order` is "give units an order (no mouse, world coords)"; `scenario apply` is "mutate a live game", compiling entities into a **spawn table** applied from a detour inside the game tick (`research/notes/scenario-format.md`); ~18 verbs act on a live instance (`keys click order wheel eye gui arm peek weapons shot glshot log roster wait ui switches scenario apply`). **Caveat:** "any command" is loose — `create`/`launch`/`rm` are not sent *to* a running game. True in spirit, slightly overclaimed literally |

**The grid is a composite, and that is a disclosure, not a detail.** The wide shot
tiles a handful of real clips across 1600 cells. It is an ordinary montage technique
and it is fine — but never describe the film as "N instances running at once" unless
N instances were actually running. If the film wants that claim, shoot it: run as many
as the machine holds, count them, and say *that* number.

### The six promo scenarios

Written 2026-09-10, one per clip id, and all six validate:

| scenario | map | entities | what the tile shows |
|---|---|---|---|
| `big-battle` | Town & Country | 1200 | four columns converging on one point |
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

### Open placeholder in `promo/tacli-promo.json`

* **The terminal output line *format* is invented.** The map names and unit counts in
  `output` are real (read back from `scenario expand`), but the surrounding layout is
  a guess at what `tacli scenario load` prints. Run it once, capture the transcript,
  and match it — a promo that shows its own tool printing something it does not print
  is the kind of detail that gets noticed.

## Capturing the clips

**Keep the terminal procedural and the game real.** The terminal's text is data in the
script, which means perfect typing timing, crisp glyphs at any zoom, and no dependency
on anyone's shell theme — but paste in the **real** transcript (rule above). Only the
game windows need footage.

Per clip, following `ta-capture` rule 5 — grab the **window**, never a screen region:

```bash
tools/tacli scenario load front1 big-battle --res 1024x768
tools/tacli ls --json                       # window[0] is the CLIENT id; display field
ffmpeg -y -f x11grab -window_id 0x<client> -draw_mouse 0 -framerate 30 \
  -video_size 1024x768 -i <display> -c:v libx264 -preset ultrafast -crf 15 \
  -pix_fmt yuv420p -t 20 clips/big-battle.mp4
tools/tacli stop front1
```

Why this and not a desktop grab:

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

Whether the engine will give ~2880×2160 has **not been checked** — determine it at
shoot time (`tacli launch --res`, and `--window` letterboxes if the engine's own
screen has to differ). If it will not, capture the heroes as large as it does allow
and accept a smaller upscale; 1920×1440 is a 1.47× upscale, which is far better than
2.76×.

Whatever you choose, keep the aspect at 4:3 and keep `stage.tile` matching it.

Shoot each clip **longer than the longest time it is on screen**, and shoot more
distinct clips than you think you need — `fill` gives each cell a random clip and a
random time offset, but six clips across 1600 tiles is still six clips, and the eye
finds the repeat.

## Encoding the deliverables

Render is CRF 18 by default, which is the master. Then:

* **Share cut** — try `-crf 25 -preset slow` at 1080p first and check the size. Grid
  content is high-frequency and compresses badly; the wireframe master is 43 MB for
  54 s and the CRF 25 cut is 24 MB. If a hard cap binds (GitHub attachments are
  10 MB on the free plan), **downscale to 720p before starving the bitrate** — fewer
  pixels to quantise beats more pixels encoded worse.
* Judge a candidate on a **wide-shot frame**, not on the number: that is where the
  cut either holds up or turns to mush.
* Verify the decode (hard rule 7) before sending anything.

## Re-cutting for a new release

This is the case the whole design exists for:

1. Re-capture the clips into a fresh directory — same commands, new build.
2. `tamontage render promo/tacli-promo.json -o master.mp4 --backend clip --clips <new>`.
3. Re-check the claim table above: a caption that was true at v0.2 may not be at v0.3.
4. Re-encode the share cut, verify the decode.

The camera, the beats and the copy are untouched. If the *timing* needs to change,
change it in `wire` first — it is a minute per iteration there and an evening here.

## Status

Built 2026-09-10. `promo/tacli-promo.json` is the 54 s tacli promo, in the **Card
treatment** — full-frame typographic cards, chosen by the owner from five prototype
cuts (`promo/prototype-cuts/`). Target output is **1080p landscape now, 4K for the
final**.

Two things the prototype changed in the base cut, both worth keeping:

* Beat 1's card moved to **after** the terminal flips to the game. Over the terminal
  it covered the command and the lines it printed — the one thing that shot exists
  to show.
* Captions wrap and shrink (above), because the card treatment typesets large.

* **`wire` pass**: renders, decodes clean, 54.0 s / 1620 frames. Reviewed as the
  prototype.
* **Stability**: wide-shot jitter 108.7 → 30.7 at the default `--supersample 3`.
* **`clip` pass**: exercised end to end against six synthetic 1024×768 clips —
  extraction, thumb cache, hero tile at full size and the 1600-tile wide shot, all
  decoding clean. Footage lands in a tile 1:1 as intended.
* **Not yet done**: no *game* footage has been shot, and the two placeholders above
  (scenario names, terminal transcript) are open. Until they are closed this montage
  is a prototype, not a promo.
