---
name: ta-capture
description: Capture screenshots and video of Total Annihilation on the tagpu stack — engine-surface shots, ffmpeg x11grab video for flicker/animation debugging, frame-by-frame analysis, marker-verified engine-vs-ours pairs. Use when asked to record, capture, film, screenshot, or frame-analyze the game. Launching and driving the game is the ta-drive skill.
---

# TA capture (tagpu project)

TA-specific rules earned in the Phase B-D
sessions. **Fold new lessons back into this file.**

**Launching and driving the game is the `ta-drive` skill** (`tools/tacli`): instances,
windowed mode, silence, keys, clicks, camera, skirmish presets. This file covers only
*observing* what a running instance draws.

There are two things to capture, and one tool for each:

* **`tacli shot`** — the engine's own 8bpp surface, on every renderer. It shows the engine's
  frame — UI, and the flat key fill inside the world viewport — so it is the right tool for
  reading the UI, the shell or a dialog, and the wrong one for judging what a pass drew.

  **Capture loading screens through the presented window.** The trigger is polled at the
  engine flip's entry, before back buffer → primary. It arms a shot that is serviced on the
  next eligible trigger-family pass, so it can miss a short-lived loading frame. Record from
  before `tacli ui <i> click Start`, keep capture timestamps, and compare image signatures.
  The shell's logical surface is 640×480; measure the actual client size for the window grab.

* **the window, grabbed by its client id** — the presented frame, our passes included. One
  frame is `import -window 0x<client>` on the instance's display; video is ffmpeg x11grab
  `-window_id` (rule 5). There is no in-process capture of the presented frame that a `tacli`
  verb reaches; the Vulkan backend's own (`tagpu_vk_shot.c`) serves the per-pass A/B levers.

Game dir: an instance's `tagpu/instances/<name>/gamedir/` (cwd of that game; the older
single-gamedir path `tagpu/gamedir/` still works for a hand-launched run). Trigger files
are created there; `tacli shot` wraps the screenshot trigger and returns the file path,
which is preferable to poking the trigger by hand.

Two facts that shape every capture:

- **NATIVE UNITS ARE INVISIBLE IN SURFACE SHOTS** (the engine draws nothing for them) —
  judge our renderer from window grabs, and engine state (menus, UI, placement boxes) from
  surface shots.
- **Copy the run's log out before it ages out**: `tools/talog.py run <gamedir> > $OUT/run.log`.
  `log/` keeps the last ten parts of each stream and deletes older ones, so numeric evidence
  such as a sub-pixel filmstrip is gone a few launches later.

## The three capture paths

| Path | Trigger | What it sees | Output |
|---|---|---|---|
| Surface shot | `tagpu_shot.trigger` | the 8bpp ENGINE frame only (none of our passes) | `<gamedir>/Screenshots/*.png` at the surface size (640×480 in the shell, the game mode in play) |
| Window grab | `import -window 0x<client>` (rule 5) | the presented frame, our passes included | png, client-sized |
| Video | ffmpeg x11grab `-window_id` (below) | the window's OWN content, occluded or not | mkv |

The trigger family has a **16 ms minimum interval**, evaluated at engine flips
(`before_flip` in `tagpu_gui_hook.c`). `tagpu_triggers_frame` services an armed surface shot
before arming a new one: capture follows on the next eligible family pass. Actual throughput
depends on engine flips, PNG encoding and the reader; there is no eight-frame capture gate
or required fixed sleep. `tacli shot` itself checks for output every 250 ms.

A custom burst loop must wait for the previous PNG to decode completely before copying or
removing it and requesting another. Names use wall-clock seconds and collision suffixes
(`_2`, `_3`, …); removing a file lets its name be reused within that second. Snapshot the
directory before each trigger and detect new output relative to that snapshot. Measure
distinct frames and simulation ticks rather than assuming each captured image advances the
game. See `research/notes/capture-workflows.md` for the recorded rates and their limits.

## Hard rules

1. **Window targeting: ask tacli, do not guess.** `tacli ls --json` gives the client
   window id and its `x, y, w, h` for each instance. Searching by name is a trap:
   `xdotool search --name "Total Annihilation"` also matches the user's browser and
   Discord windows, and each instance itself owns two windows (frame + client).
   `window[0]` of the `tacli ls --json` entry is the **client** id — that is the one
   `-window_id` wants (rule 5); the `x, y, w, h` beside it are the FRAME's.
2. **Never inject keys or clicks with xdotool** — use `tacli keys` / `tacli click`
   (in-process, reliable, session-state-proof). X injection has landed keystrokes in
   the user's unlock dialog. xdotool stays fine for reading geometry and for capture.
3. **Keep each window inside its own display.** `tacli`'s tiler reads that display's size
   and wraps into overlapping cells when it runs out (`tile_for` in `tools/tacli`). Keep
   hand placement inside those bounds. An isolated display belongs to the experiment;
   a game handed to the human must be visible on their monitor.
4. **Measure the client geometry.** A default windowed launch follows the game resolution,
   but `--window` and fullscreen can make the client a different size. Read the client with
   `xwininfo -id <client>` on the instance's display and use that size for `-video_size`.
   Recalibrate any game-to-window coordinates after a display-mode or resolution change.
5. **Recording video: grab the WINDOW, not a screen region** (2026-09-08).
   `-window_id` reads the window's own redirected pixmap, so the capture is the game's
   frame no matter what is stacked over it, wherever the window sits — and the user
   keeps their desktop. Take the **client** id from `tacli ls --json` (rule 1):

   ```bash
   ffmpeg -y -f x11grab -window_id 0x<client> -draw_mouse 0 -framerate 60 \
     -video_size <w>x<h> -i <display> \
     -c:v libx264rgb -preset ultrafast -qp 0 -t <secs> out.mkv
   ```

   Measured at 1920×1080 with the game window fully covered by another application and
   stacked `below` everything: against an in-process capture of the same frame the side
   panel and the top bar differ by **0.000 %**, the viewport by 0.02 % — which is the
   fraction of a second between the two captures, not the capture. Take `<display>`
   from `tacli ls --json`'s `display` field; it is per session (`:0` here, `:1` in the
   2026-09-02 notes below) and hard-coding it grabs nothing.

   **A region grab of a covered window records the covering window, silently.** The
   first attempt this way came back 97 % exact-duplicate frames with a 216-px maximum —
   which reads as a frozen game, not as a wrong target, and the game was presenting at
   a measured 60 fps throughout. The tell is the duplicate rate: decode the clip and
   count frames identical to their predecessor. While the unit walked, the `-window_id`
   grab ran **4 %** duplicates; anything near 50 % is the 30 Hz sim showing through a
   still scene, anything near 100 % is the wrong window. **`xdotool windowraise` does
   not fix it** — mutter ignores raise requests from another client (activating instead
   would steal the user's focus, so do not). Move the window somewhere harmless and grab
   it by id.

   **Lossless, for flicker work**: `libx264rgb -qp 0` costs ~1.8 MB/s at 1080p60 on game
   content and keeps a one-frame artifact from being smeared by the encoder, which
   matters when the detector thresholds on small differences.
   **CAPTURE AT 60 fps** for flicker hunts — the present runs at 60 Hz and a
   one-present dropout (16 ms) aliases invisibly into a 30 fps capture (this
   exact miss produced two false "all clean" verdicts). The user seeing an
   artifact your 30 fps analysis doesn't show = raise the framerate first.
   Surface screenshots can't catch present-level artifacts at all (they read
   the 8bpp surface, not what's displayed) — video is the ground truth.
   `-draw_mouse 0`: the game draws its own cursor sprite, so X's pointer in the frame is
   a second cursor and a moving false positive.

   **If you must grab a screen region anyway** (2026-09-02, still true): the session must
   be unlocked and actually displaying the window. **A region of the live desktop can include
   other applications**, so say so before you record. `LockedHint` is **not** a sufficient check: a
   switched-away session reports `LockedHint=no` while GNOME's `mutter guard window`
   (full-screen, `IsViewable`) covers everything, and the capture comes back as blurred
   wallpaper. Test with one `-frames:v 1 -update 1` grab diffed against a `-window_id` grab of the same window;
   a mean abs difference of ~1 means you have the window, ~40 the guard. And the client
   rect from `xwininfo -id <client>` (`Absolute upper-left`) is the one to grab, **not**
   the `tacli ls --json` rect, which is the frame — they differed by (14, 49) here.
   **Validate the image before trusting a locked-session capture.** `xwd -id`,
   `import -window` and ffmpeg `-window_id` can produce black frames while the game keeps
   running. A valid file or normal byte count does not establish visible content.
   For engine UI, `tacli shot` remains an independent surface path. For Vulkan output,
   use an isolated display and validate a presented-frame grab there; a surface shot
   cannot show our GPU passes. See `research/notes/capture-workflows.md` for the limits.
   **A desktop flash during a mode change needs an isolated-display grab.** A
   `-window_id` capture cannot show the desktop uncovered by the window shrinking.
   On an isolated display, give the root a known background colour and record the
   whole display at 60 fps; keep the normal window-only rule on the human's desktop.
   Pair the recording with timestamped window Create/Destroy, Map/Unmap and Configure
   events. Keep ConfigureNotify's own width/height: querying the window after the
   event can already see the next size and hide the intermediate one. See
   `research/notes/fullscreen-handoff.md` for the reproduction and its compositor gap.

6. **Marker-verified pairs** (engine vs ours): `echo "STRIP <phase> $(date +%s)"
   >> $OUT/markers.txt` before each capture, match PNG mtimes with
   `ls --time-style=+%s`. The marker goes in your own file, never into `log/tagpu.log`: the DLL
   is that file's only writer and its caps count only its own bytes. Blind alternation mislabels rows. Compose panels with
   `tools/sbs.py out.png label1 img1 label2 img2` (needs Pillow).
7. **Establish the noise floor before trusting any A/B number.** An engine-vs-ours
   diff is only meaningful on a frozen scene: capture the SAME arm state twice, a
   few seconds apart, and diff those first. It must come out **zero**. A walking
   unit, a growing LOS blob or a still-scrolling camera moves the fog boundary and
   the health bars between the two halves and shows up as thousands of "mismatched"
   pixels that look exactly like a rendering bug. This cost a wrong diagnosis in the
   G13c fog gate: 19,768 px of apparent error, chased into the shader, when the real
   reading was 97 once the commander had parked. Park everything (`tacli eye` to pin
   the camera, let orders finish), confirm the zero, then run the A/B.
   **And separate the confounders you cannot freeze.** Some differences are
   permanent features of the frame, not of the change under test — the fog edge is
   the standing example: the engine dithers its edge sprites and we threshold
   cleanly, so every fogged A/B carries a 2–4 px band of "error" along the whole
   boundary. In G13b that read as 0.91 % of the viewport and looked like a terrain
   bug; the tell was that it was **the same number at four unrelated camera
   positions** (a real geometry error scales with content). Either turn the
   confounder off — note that the `LineOfSight` setting is sticky per instance and
   will carry over from a previous launch — or dilate the difference mask by ~8 px
   and measure what survives outside it. Here that left 90.6 % of the viewport with
   **zero** differing pixels, which is the number worth reporting.
8. **`grep -a` on `log/tagpu.log`, always** — stray binary bytes make grep treat it
   as a binary file and silently print nothing.
9. **Sim speed**: TA's own `+`/`-` keys via `tacli keys <name> plus` / `minus`
   (up to +10, down to −9; minus stretches builds ~10× for capture). Verify
   empirically (sample a log value twice, 15 s apart) before trusting a change.
10. **Camera steering**: `tacli eye <name> X Y` pins the eye (it writes both the eye
   and the scroll target, so the engine stops fighting), `--release` frees it. Steer by
   the roster log (`u### TYPE own world screen`, every 5 s (landing 10c-3; it was every 300 render frames)): roster `screen=`
   already includes the viewport offsets, and unit ids are NOT stable (alive-counter
   order) — match by world coords. Mouse-edge scrolling is legacy; do not warp the
   user's pointer to scroll.

## Closed-loop camera steering (converges in ~3 rounds)

The `units:` log line carries a fresh `eye=(x,y)` every 500 ms (landing 10c-3; it was every 30 render frames, which is 0.5 s at 60 fps — this line used to say ~1 s and was wrong before the move too) — use
it as feedback instead of the roster/swept lines (up to 5 s stale = blind
overshoot: that block runs on `ROSTER_DUMP_MS` = 5 s since the same landing, and
the “10 s” this said was not right under the old 300-frame gate either — 300
frames at 60 fps is 5 s). Loop: read eye → scroll toward target with a duration computed from
the delta (`~1400 px/s`, clamp 0.15–3 s) → park → re-read. Break when within
200 px. See the memory log for the exact bash loop (2026-09-01 entry).

## Frame-by-frame flicker analysis (the recipe that caught the mex dropout)

1. Record 6 s per rule 5.
2. Crop the suspect unit region every frame:
   `ffmpeg -i in.mp4 -vf "crop=500:460:<winx>:<winy>" frames/f%03d.png`
3. Metric per frame — count "structure" pixels (grey/bright: `r>90 and b>90
   and |r−b|<60` on every 3rd pixel), then flag frames <70% of the average;
   assemble suspect±1 frames into a strip with Pillow and eyeball them.
4. A unit vanishing = a frame the Vulkan unit pass stood down on (the engine's
   own copy does not reach the Vulkan frame, so nothing covers it —
   `tagpu_owndraw_classify`'s note); pieces missing = partial render; a pose that is **wrong for one frame and right on both
   neighbours** is the render thread reading the engine's posed vertex buffer
   while the game thread rewrites it (gpu-status §2.9) — not aliasing, and not
   cosmetic: at rest orientation it is ~1400 changed pixels at 2× zoom.

5. **A transient-ranked frame is a candidate, not a finding.** The useful
   detector is "differs from both neighbours while the neighbours agree with
   each other" — it rejects smooth motion and animated water — but a unit
   *rotating* passes it too, because frame k−1 and k+1 can resemble each other
   while k sits between them. Every ranked frame up to ~600 changed pixels on a
   walking commander at 2× turned out to be ordinary yaw or a leg swing. Open
   the strip before believing the number, and prefer an in-DLL oracle over a
   pixel threshold whenever the bug has one.

6. **Gate C is shipped, not reconstructed each time.** The pose race's own
   regression — the fixture, the pressure rig, the capture and the detector —
   is `scenarios/walk-gatec.json` + `tools/gatec.sh` + `tools/gatec_detect.py`,
   run once per arm state on one instance. It exists because the G13 runs were
   driven by hand and their leg geometry was never recorded, so the `> 500`
   band could not be reproduced when G16 re-ran the protocol (the replacement
   fixture reaches 520 px on the guarded path that ships). **The transferable
   bar is `> 1000` → 0**; the lower bands are the walk itself and belong to the
   fixture. `research/notes/gpu-posing.md` §4 step 7.

7. **NEVER key a detector on a hard-coded RGB.** `Gamma` is one shared mutable
   registry inode across every instance (memory: the G17a landing), so another
   session moving it repaints our whole palette: the health-bar legs came back
   with the GUI green at `(83,223,79)` where the detector's constant said
   `(93,250,88)`, an exact match found **zero** pixels in every frame of both
   builds, and the tool reported "no usable run" — a silent total miss that
   looks exactly like a capture that failed. Calibrate the key from the capture
   itself (`barwobble_detect.calibrate_green`: the modal colour above a
   saturation threshold, since the GUI green is far more saturated than
   terrain), and it survives the next session that touches Gamma.

8. **Measure the thing against ITSELF before measuring it against a neighbour.**
   The health-bar oracle referenced the bar to the selection box, which is a
   rotated outline whose centroid breathes ~11 px at 4×; a defect that made the
   bar teleport 4 px a frame moved that statistic only 2.79 → 2.27 and read as
   a pass. The bar's own second difference caught it outright (4.00 → 1.00 px
   p99) because a unit walks at constant speed, so any anchored thing that
   tracks it has a second difference near zero. A reference that has its own
   motion is a noise source, not a baseline.

9. **A defect worth `zoom` pixels needs a leg at a zoom.** Anything this stack
   emits unzoomed is scaled by the vertex shader, so a quantisation error in
   pre-zoom units is at the measurement floor at 1× and `ZOOM_MAX` = 8 px at
   full zoom-in. Every 1× leg of the health-bar A/B passed while the owner was
   watching the bar jump across the screen. Fixtures: the 1× walk leaves the
   viewport entirely at 4×, so the zoomed leg needs its own camera and a
   shorter walk (`scenarios/bar-wobble-4x.json`), and every length in a
   detector — run thresholds, anchor offsets, search windows — scales with it.

10. **A race needs scheduling pressure, not a longer run.** A window that is
   microseconds wide and opens tens of times a second is essentially never
   sampled on an idle machine: a minute of walking caught it zero times
   over four runs. `taskset -acp 0 <pid>` on the game plus two or three
   spinners pinned to the same core makes its own render and game threads
   timeshare — which is what a loaded machine does to a player — and brought
   the same walk to five to twelve catches a minute. A single core is used,
   and nothing else on the machine is touched; unpin afterwards.

## A zoomed close-up that follows a unit

- Use a private `Xvfb :N -screen 0 3840x2160x24` and create your own instance on `:N`.
  Whole-display `ffmpeg -f x11grab -i :N+0,0` is appropriate there because the display
  contains only your experiment. Validate actual frames; an isolated display removes the
  human desktop's covering windows, but does not establish a rendering rate.
- Set the simulation speed on the stopped instance with `tacli registry <i> gamespeed=1`
  if slow motion is needed. A newly seeded store starts at 10; existing stores retain their
  value. Measure simulation ticks and presented frames separately. Encoding at 30 fps
  does not make a low-rate simulation advance at 30 ticks/s.
- Write `tagpu_zoom.txt` atomically; the active lever is reread each render frame. Calibrate
  the game viewport centre and world-to-screen mapping for the current HUD, resolution and
  zoom instead of reusing coordinates from another recording.
- `tacli eye <i> X Y` holds the camera. The hold's presence is checked every 15 render frames;
  once active, its integer position is reread every render frame and applied on the game
  thread before drawing. Removal invalidates the hold on the next active read. Measure the
  screen-space steps when tracking at high zoom; a pinned camera and an offline crop are
  useful when integer world coordinates are too coarse for the shot.
- Park the in-game cursor over a safe sidebar point with
  `tacli keys <i> pmove:<x>,<y> pmove:<x>,<y>`; the repeated token handles the keys verb's
  first-token behaviour. Never move the human's pointer. Judge laser size and motion from
  the actual presented capture at the chosen zoom.

## Launching (see the ta-drive skill for the full driving surface)

```bash
tools/tacli launch cap1 --res 1024x768     # ~2s to a window, silent, no intro
tools/tacli ls --json                      # window rect for x11grab
tools/tacli stop cap1
```

Wedged (menu keys dead) = `tacli stop <name> --hard` and relaunch. A DLL rebuild is
picked up by the next `tacli launch`. Sandbox note: run game-dir reads/writes and
wine/ffmpeg commands with the sandbox disabled.

## Maintaining this skill

State current procedures in the present tense. Replace incorrect instructions instead of
appending a correction. Keep dated measurements and their limits in
`research/notes/capture-workflows.md` or the relevant investigation note. Check each named
lever and tool against the source, and verify the rendered wiki when changing its notes.
