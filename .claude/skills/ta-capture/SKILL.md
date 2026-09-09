---
name: ta-capture
description: Capture screenshots and video of Total Annihilation on the tagpu stack — surface shots, GL-framebuffer shots, ffmpeg x11grab video for flicker/animation debugging, frame-by-frame analysis, marker-verified engine-vs-ours pairs. Use when asked to record, capture, film, screenshot, or frame-analyze the game. Launching and driving the game is the ta-drive skill.
---

# TA capture (tagpu project)

TA-specific rules earned in the Phase B-D
sessions. **Fold new lessons back into this file.**

**Launching and driving the game is the `ta-drive` skill** (`tools/tacli`): instances,
windowed mode, silence, keys, clicks, camera, skirmish presets. This file covers only
*observing* what a running instance draws.

Game dir: an instance's `tagpu/instances/<name>/gamedir/` (cwd of that game; the older
single-gamedir path `tagpu/gamedir/` still works for a hand-launched run). Trigger files
are created there; `tacli shot` / `tacli glshot` wrap the two screenshot triggers and
return the file path, which is preferable to poking triggers by hand.

Two facts that shape every capture:

- **NATIVE UNITS ARE INVISIBLE IN SURFACE SHOTS** (the engine draws nothing for them) —
  judge our renderer from GL shots, and engine state (menus, UI, placement boxes) from
  surface shots.
- **Archive `tagpu.log` before relaunching** (`mv tagpu.log tagpu.runN.log`): numeric
  evidence such as a sub-pixel filmstrip dies with an `rm`.

## The three capture paths

| Path | Trigger | What it sees | Output |
|---|---|---|---|
| Surface shot | `tagpu_shot.trigger` | the 8bpp ENGINE frame only (pre-GL, no overlays) | `<gamedir>/Screenshots/*.png` at the surface size (640×480 in the shell, the game mode in play). **Broken in game between the window-title landing and 2026-09-07**: the title's `:` and `\|` made an illegal filename and the PNG never appeared; fixed in `screenshot.c` |
| GL shot | `tagpu_glshot.trigger` | the composed GL frame incl. our overlays | `tagpu/gamedir/tagpu_gl.ppm`, window-sized |
| Video | ffmpeg x11grab `-window_id` (below) | the window's OWN content, occluded or not | mkv |

Surface trigger fires at most once per 8 frames (~0.26 s) — burst loops need
`sleep 0.55` between triggers.

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
3. **A `glshot` is always whole.** It used to come back mangled — the game in the
   bottom-left corner of an otherwise garbage frame — whenever the window was not fully
   on the visible desktop, because `glReadPixels` on the default framebuffer is
   undefined outside the region that passes the pixel-ownership test, and `tacli`'s
   tiler used to march windows off the bottom of the screen. Both are fixed (the tiler
   reads the real display size and wraps; the capture owns its target), so a wrong-
   looking `glshot` is now evidence of a real rendering bug, not of window placement.
4. **Geometry: windowed instances are 1:1** — the client area *is* the game
   resolution, so game px == window px and no letterbox maths is needed, and
   `-video_size <w>x<h>` is simply the instance's `res` (grab it by window id, rule 5).
   `tagpu_gl.ppm` is the same size as the client area. (Legacy fullscreen runs letterbox 4:3 inside 3840×2160 at
   `+480,0`, scale 4.5, i.e. game px → window px = `(480 + gx*4.5, gy*4.5)`; those
   numbers are 640×480-only and must be recalibrated after any resolution change.)
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
   stacked `below` everything: against a `tacli glshot` of the same instance the side
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
   **CAPTURE AT 60 fps** for flicker hunts — the GL present runs at 60 Hz and a
   one-present dropout (16 ms) aliases invisibly into a 30 fps capture (this
   exact miss produced two false "all clean" verdicts). The user seeing an
   artifact your 30 fps analysis doesn't show = raise the framerate first.
   Surface screenshots can't catch present-level artifacts at all (they read
   the 8bpp surface, not what's displayed) — video is the ground truth.
   `-draw_mouse 0`: the game draws its own cursor sprite, so X's pointer in the frame is
   a second cursor and a moving false positive.

   **If you must grab a screen region anyway** (2026-09-02, still true): the session must
   be unlocked and actually displaying the window. **The user's desktop is REAL**
   (6200×2160, with their browser, Discord, Zoom and terminals on it) — not an isolated
   display — so say so before you record. `LockedHint` is **not** a sufficient check: a
   switched-away session reports `LockedHint=no` while GNOME's `mutter guard window`
   (full-screen, `IsViewable`) covers everything, and the capture comes back as blurred
   wallpaper. Test with one `-frames:v 1 -update 1` grab diffed against a `tacli glshot`;
   a mean abs difference of ~1 means you have the window, ~40 the guard. And the client
   rect from `xwininfo -id <client>` (`Absolute upper-left`) is the one to grab, **not**
   the `tacli ls --json` rect, which is the frame — they differed by (14, 49) here.
   **`xwd -id <window>` returns full window content even while the session is locked**
   (3.1 MB for 1024×768, verified) — the escape hatch when the screen is unavailable, and
   the reason an in-process `glReadPixels` recorder would make locked 60 Hz capture
   possible.
   **None of this applies to `tacli glshot` (2026-09-02).** It renders the frame into an
   FBO the fork owns and reads *that*, so it never touches the window's back buffer and is
   correct whether the window is off-screen, covered, or the session is locked or switched
   away. Verified by moving an instance 49 px off the bottom of the desktop: **0 differing
   pixels** in the rows that were off-screen. It is the reference a video grab is checked
   against, and the thing to reach for first when the screen is unavailable.
6. **Marker-verified pairs** (engine vs ours): `echo "STRIP <phase> $(date +%s)"
   >> tagpu/gamedir/tagpu.log` before each capture, match PNG mtimes with
   `ls --time-style=+%s`. Blind alternation mislabels rows. Compose panels with
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
8. **`grep -a` on tagpu.log, always** — stray binary bytes make grep treat it
   as a binary file and silently print nothing.
9. **Sim speed**: TA's own `+`/`-` keys via `tacli keys <name> plus` / `minus`
   (up to +10, down to −9; minus stretches builds ~10× for capture). Verify
   empirically (sample a log value twice, 15 s apart) before trusting a change.
10. **Camera steering**: `tacli eye <name> X Y` pins the eye (it writes both the eye
   and the scroll target, so the engine stops fighting), `--release` frees it. Steer by
   the roster log (`u### TYPE own world screen`, every 300 frames): roster `screen=`
   already includes the viewport offsets, and unit ids are NOT stable (alive-counter
   order) — match by world coords. Mouse-edge scrolling is legacy; do not warp the
   user's pointer to scroll.

## Closed-loop camera steering (converges in ~3 rounds)

The `units:` log line carries a fresh `eye=(x,y)` every 30 frames (~1 s) — use
it as feedback instead of the 300-frame roster/swept lines (10 s stale = blind
overshoot). Loop: read eye → scroll toward target with a duration computed from
the delta (`~1400 px/s`, clamp 0.15–3 s) → park → re-read. Break when within
200 px. See the memory log for the exact bash loop (2026-09-01 entry).

## Frame-by-frame flicker analysis (the recipe that caught the mex dropout)

1. Record 6 s per rule 5.
2. Crop the suspect unit region every frame:
   `ffmpeg -i in.mp4 -vf "crop=500:460:<winx>:<winy>" frames/f%03d.png`
3. Metric per frame — count "structure" pixels (grey/bright: `r>90 and b>90
   and |r−b|<60` on every 3rd pixel), then flag frames <70% of the average;
   assemble suspect±1 frames into a strip with Pillow and eyeball them.
4. A unit vanishing = empty-composite flicker (see r3dcache); pieces missing =
   partial render; a pose that is **wrong for one frame and right on both
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

6. **A race needs scheduling pressure, not a longer run.** A window that is
   microseconds wide and opens tens of times a second is essentially never
   sampled on an idle machine: a minute of walking caught it zero times
   over four runs. `taskset -acp 0 <pid>` on the game plus two or three
   spinners pinned to the same core makes its own render and game threads
   timeshare — which is what a loaded machine does to a player — and brought
   the same walk to five to twelve catches a minute. A single core is used,
   and nothing else on the machine is touched; unpin afterwards.

## Launching (see the ta-drive skill for the full driving surface)

```bash
tools/tacli launch cap1 --res 1024x768     # ~2s to a window, silent, no intro
tools/tacli ls --json                      # window rect for x11grab
tools/tacli stop cap1
```

Wedged (menu keys dead) = `tacli stop <name> --hard` and relaunch. A DLL rebuild is
picked up by the next `tacli launch`. Sandbox note: run game-dir reads/writes and
wine/ffmpeg commands with the sandbox disabled.
