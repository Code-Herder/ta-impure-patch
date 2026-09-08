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
| Video | ffmpeg x11grab (below) | the live display incl. everything | mp4 |

Surface trigger fires at most once per 8 frames (~0.26 s) — burst loops need
`sleep 0.55` between triggers.

## Hard rules

1. **Window targeting: ask tacli, do not guess.** `tacli ls --json` gives the client
   window id and its `x, y, w, h` for each instance. Searching by name is a trap:
   `xdotool search --name "Total Annihilation"` also matches the user's browser and
   Discord windows, and each instance itself owns two windows (frame + client).
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
   resolution, so game px == window px and no letterbox maths is needed. Take the rect
   from `tacli ls --json` and grab it directly:
   `ffmpeg -f x11grab -video_size <w>x<h> -i :1+<x>,<y> ...`. `tagpu_gl.ppm` is the same
   size as the client area. (Legacy fullscreen runs letterbox 4:3 inside 3840×2160 at
   `+480,0`, scale 4.5, i.e. game px → window px = `(480 + gx*4.5, gy*4.5)`; those
   numbers are 640×480-only and must be recalibrated after any resolution change.)
5. **Recording video**: the session must be unlocked and actually displaying the
   window. **`:1` is the user's REAL desktop** (6200×2160, with their browser, Discord,
   Zoom and terminals on it) — not an isolated display — so an x11grab region records
   their screen, and anything stacked over the game window lands in the video. Grab the
   game's rect only, and say so before you record.
   `LockedHint` is **not** a sufficient check: a switched-away session reports
   `LockedHint=no` while GNOME's `mutter guard window` (full-screen, `IsViewable`) covers
   everything, and the capture comes back as blurred wallpaper. Test first — one
   `-frames:v 1 -update 1` grab, diffed against a `tacli glshot` of the same instance;
   a mean abs difference of ~1 means you have the window, ~40 means you have the guard.
   Two more things learned the hard way (2026-09-02):
   - the client rect from `xwininfo -id <client>` (`Absolute upper-left`) is the one to
     grab, **not** the `tacli ls --json` rect, which is the frame — they differed by
     (14, 49) here;
   - **`xwd -id <window>` returns full window content even while the session is
     locked** (3.1 MB for 1024×768, verified). It is not a 60 Hz path on its own, but it
     is the escape hatch when the screen is unavailable, and it is the reason an
     in-process `glReadPixels` recorder would make locked 60 Hz capture possible.
   **None of this applies to `tacli glshot` any more (2026-09-02).** It renders the frame
   into an FBO the fork owns and reads *that*, so it never touches the window's back
   buffer and is correct whether the window is off-screen, covered, or the session is
   locked or switched away. Verified by moving an instance 49 px off the bottom of the
   desktop: **0 differing pixels** in the rows that were off-screen. It is therefore the
   reference a video grab is checked against, and the thing to reach for first when the
   screen is unavailable.
   Capture the instance rect from `tacli ls --json`:
   `ffmpeg -y -f x11grab -framerate 60 -video_size <w>x<h> -i :1+<x>,<y>
    -c:v libx264 -preset ultrafast -crf 18 -pix_fmt yuv420p -t <secs> out.mp4`
   **CAPTURE AT 60 fps** for flicker hunts — the GL present runs at 60 Hz and a
   one-present dropout (16 ms) aliases invisibly into a 30 fps capture (this
   exact miss produced two false "all clean" verdicts). The user seeing an
   artifact your 30 fps analysis doesn't show = raise the framerate first.
   Surface screenshots can't catch present-level artifacts at all (they read
   the 8bpp surface, not what's displayed) — video is the ground truth.
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
   partial render; pose jumps = temporal aliasing (one-frame lag, cosmetic).

## Launching (see the ta-drive skill for the full driving surface)

```bash
tools/tacli launch cap1 --res 1024x768     # ~2s to a window, silent, no intro
tools/tacli ls --json                      # window rect for x11grab
tools/tacli stop cap1
```

Wedged (menu keys dead) = `tacli stop <name> --hard` and relaunch. A DLL rebuild is
picked up by the next `tacli launch`. Sandbox note: run game-dir reads/writes and
wine/ffmpeg commands with the sandbox disabled.
