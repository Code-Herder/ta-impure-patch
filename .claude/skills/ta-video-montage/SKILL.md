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
4. **Tile detail must scale with tile size.** In the wide shot a tile is ~45 px, and
   a 1 px white border on 45 px is 4 % of the tile: the grid stops reading as a wall
   of running games and starts reading as woven fabric. Below `detail` (90 px) the
   outline and the internal grid come off and the desktop gap does the separating.
   Judge this on a wide-shot still, never on a close one.
5. **The content cache is per frame, keyed by (source, frame, pixel size).** Tile
   size changes every frame during a zoom, so a cache that lives across frames only
   thrashes. Within one frame every tile at a given zoom is the same size, so the
   grid costs a few dozen renders rather than 1600.
6. **Render cost is dominated by big tiles, not by many tiles.** The 1600-tile wide
   shot and the single-terminal opening cost about the same. Do not optimise the grid
   before measuring.
7. **Treat every encode as unverified until it decodes end to end.** `ffmpeg -v error
   -i out.mp4 -f null -` must print nothing. Check the duration and frame count too.
8. **Importing `tools/tamontage` from a script needs `SourceFileLoader`** — it has no
   `.py` extension, so `spec_from_file_location` returns a spec with no loader and
   fails with a bare `'NoneType' object has no attribute 'loader'`.
9. **Extracting clip frames: `-r <fps>` alone, never with `-vsync`/`-fps_mode`.**
   ffmpeg refuses the pair — *"One of -r/-fpsmax was specified together a non-CFR
   -vsync/-fps_mode. This is contradictory."* — and CFR is what the renderer needs
   anyway, because `Clip.frame()` maps time to a frame index by multiplication.
   A variable-rate extraction silently desynchronises every tile.
10. **The thumb ladder is cached to `.npy`, and that cache is the difference between
   a 4-minute render and a 25-minute one.** Every clip frame is decoded and
   downscaled at startup — 600 decodes per clip on a 20 s capture, paid on *every*
   run. Measured on six synthetic clips: startup fell from ~60 s to ~2 s once
   `thumbs-<W>.npy` existed. Delete the cache directory after re-capturing, or the
   render uses the old footage without saying so.

### Measured cost (2026-09-10, six 1024×768 clips, 1920×1080 output)

| pass | per frame | full 54 s |
|---|---|---|
| `wire` | ~70 ms | ~72 s |
| `clip`, warm caches | ~140 ms | ~4 min |

The single-terminal opening and the 1600-tile wide shot cost about the same, so the
grid is not what to optimise (hard rule 6). Cold, add one-time frame extraction plus
the thumb build.

## Honesty: what the film may and may not claim

A caption is a claim, and this repo's rule is that claims get checked against the
source before they ship. The montage script carries the copy; **this table carries the
evidence**, and a caption with no row here does not go in the film.

| caption | status | evidence |
|---|---|---|
| "One command." / "no menu, no map setup, no clicking" | VERIFIED | `tacli scenario load` is one command and is documented as "launch a game and put the situation in it" (`tools/tacli`, `scenario load` parser) |
| "silent · windowed · no intro movies" | VERIFIED | `tacli create` parser: `--sound` is "enable sound (default: silent)", `--intro` is "keep the intro movies" — both off unless asked |
| "A 1997 engine, mid-battle, in seconds." | VERIFIED | `ta-capture`: `tacli launch cap1 --res 1024x768` is "~2s to a window"; the scenario applier runs after |
| "Every fight is a file." / `scenarios/<name>.json` | VERIFIED | `scenarios/*.json`, 57 of them; `scenario validate` / `expand` / `apply` / `load` |
| "Instances share nothing." / "own game dir · own config · own window" | VERIFIED | each instance is `tagpu/instances/<name>/gamedir/` with its own config and window (`ta-capture`, `ta-drive`) |
| "Run one." / "or as many as the machine will hold" | **UNMEASURED** | nobody has yet measured how many instances this machine actually sustains. Either measure it and say the number, or keep the wording aspirational — do not put a count on screen that has not been run |
| "A test harness that happens to look like a war." | opinion | a characterisation, not a measurement; fine as the closing line |

**The grid is a composite, and that is a disclosure, not a detail.** The wide shot
tiles a handful of real clips across 1600 cells. It is an ordinary montage technique
and it is fine — but never describe the film as "N instances running at once" unless
N instances were actually running. If the film wants that claim, shoot it: run as many
as the machine holds, count them, and say *that* number.

### Open placeholders in `promo/tacli-promo.json`

Both must be closed before a real render — they are invented text standing in for real
text, which is exactly the kind of thing that ships by accident:

* **The scenario names do not exist.** `big-battle`, `air-war`, `last-stand`,
  `naval-push`, `ridge-assault`, `shore-raid` are not in `scenarios/`. Either write
  them (they are JSON, and a promo deserves scenarios chosen to look good) or re-point
  the script at real ones — `500v500`, `200v200` and `warlordex-vs-fleet` already
  carry their own drama.
* **The terminal output lines are invented.** `"applied 412 units, 2 players"` is
  plausible, not real. Run the command, capture what it prints, paste the real
  transcript into `output`.

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

**Tile size and clip size must match**, or every tile is a resample: `stage.tile` is
1024×768 because that is what `--res 1024x768` gives, and windowed instances are 1:1
(`ta-capture` rule 4). Change one, change the other.

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

Built 2026-09-10. `promo/tacli-promo.json` is the 54 s tacli promo.

* **`wire` pass**: renders, decodes clean, 54.0 s / 1620 frames. Reviewed as the
  prototype.
* **`clip` pass**: exercised end to end against six synthetic 1024×768 clips —
  extraction, thumb cache, hero tile at full size and the 1600-tile wide shot, all
  decoding clean. Footage lands in a tile 1:1 as intended.
* **Not yet done**: no *game* footage has been shot, and the two placeholders above
  (scenario names, terminal transcript) are open. Until they are closed this montage
  is a prototype, not a promo.
