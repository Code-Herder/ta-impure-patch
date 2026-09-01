---
name: ta-capture
description: Capture screenshots and video of Total Annihilation running under wine on the tagpu stack — surface shots, GL-framebuffer shots, ffmpeg x11grab video for flicker/animation debugging, frame-by-frame analysis, marker-verified engine-vs-ours pairs. Use when asked to record, capture, film, screenshot, or frame-analyze the game.
---

# TA capture & driving (tagpu project)

Adapted from an earlier capture skill; TA-specific rules earned in the
Phase B–D sessions. **Fold new lessons back into this file.**

Game dir: `tagpu/gamedir/` (symlink mirror; cwd of the running game). All
trigger files are created there (`: > tagpu/gamedir/<file>`).

## IN-PROCESS DRIVING (2026-09-01 — PREFER THIS OVER X INJECTION)

X-level injection (xdotool keys, XSendEvent, SendInput buttons/relative moves)
is UNRELIABLE-TO-DANGEROUS: a locked or half-dead GNOME session holds a server
grab — keys vanish or land in the user's UNLOCK DIALOG (this really happened:
a gdm auth failure from injected keys; see memory `ta-input-injection-safety`).
The fork now drives the game from INSIDE the process — works under any
lock/session state:

- `tagpu_keys.txt` (gamedir, consumed once ~4×/s): whitespace tokens
  `a..z 0..9 space return escape plus minus up down left right tab f1..f12`
  (posted WM_KEYDOWN/UP — menus AND in-game), `pclick:GX,GY` / `prclick:GX,GY`
  (posted click at GAME px — the ONLY reliable click; it auto-parks the game
  mouse at view centre afterwards, else edge-scroll war), `mouselock` (call
  once in-game: cnc-ddraw's wndproc DROPS all mouse messages until its
  mouse_lock() runs — the historical "clicks never work under lock" cause),
  `keydown:VK`/`keyup:VK`, `char:C`, `mouserel/mouse/click/rclick` (SendInput
  variants — DO NOT USE, they die under the shield), `ctrl+X` (posted combo —
  TA ignores it, modifier state is polled; self-destruct is NOT scriptable yet).
- `tagpu_eye.txt` "X Y" (world px): camera eye written every present frame
  while the file exists. CAVEAT: the engine re-scrolls each tick if its OWN
  scroll state disagrees (game mouse near an edge = permanent war); the log
  line `eye: WAR engine=(..) hold=(..)` exposes it; the `units:` log samples
  the eye AFTER our write and hides the war. Park the game mouse centrally
  (any pclick does) and keep the X cursor off screen edges too.
- Orders need registry `Interface Type=1` (right-mouse orders): select with
  `pclick` on the unit, order with `prclick` on the target. Classic type 0's
  left-click-order scheme resists posted clicks.
- NATIVE UNITS ARE INVISIBLE IN SURFACE SHOTS (engine draws nothing for
  them) — verify native-unit state via GL shots; verify ENGINE state
  (menus/UI/placement boxes) via surface shots.
- Archive `tagpu.log` before relaunching (`mv tagpu.log tagpu.runN.log`) —
  numeric evidence (e.g. the sub-pixel anchor filmstrip) dies with `rm`.

## The three capture paths

| Path | Trigger | What it sees | Output |
|---|---|---|---|
| Surface shot | `tagpu_shot.trigger` | the 8bpp ENGINE frame only (pre-GL, no overlays) | `tagpu/gamedir/Screenshots/*.png`, 640×480 |
| GL shot | `tagpu_glshot.trigger` | the composed GL frame incl. our overlays | `tagpu/gamedir/tagpu_gl.ppm`, window-sized |
| Video | ffmpeg x11grab (below) | the live display incl. everything | mp4 |

Surface trigger fires at most once per 8 frames (~0.26 s) — burst loops need
`sleep 0.55` between triggers.

## Hard rules

1. **Window targeting: exact title match only.** `xdotool search --name "Total
   Annihilation"` MATCHES BROWSER TABS (e.g. a "Total Annihilation Universe -
   Discord" window) and keys then go to the user's apps. Always filter:
   `for w in $(xdotool search --name "Total Annihilation"); do
      [ "$(xdotool getwindowname $w)" = "Total Annihilation" ] && W=$w && break; done`
2. **Keys: XTEST after windowactivate** (`xdotool windowactivate $W; xdotool
   key a` as throwaway first — the first key after activate often drops).
   `key --window` (XSendEvent) has stopped working in some sessions. Any key
   skips the intro movie; Escape is the conventional choice.
3. **Geometry (640×480 fullscreen on the 4K monitor).** The monitor is at
   desktop offset `+1080,0` (TRUST XRANDR, not wmctrl). Window = 3840×2160.
   The 4:3 game content is letterboxed INSIDE the window at `+480,0`, size
   2880×2160, scale 4.5 (desktop-coords: x[1560..4440]).
   - x11grab of the game: `-video_size 3840x2160 -i :1.0+1080,0`, crop
     `2880:2160:480:0` in post (window coords, NOT desktop coords).
   - `tagpu_gl.ppm` is window-sized: game content at `+480,0` there too.
   - game px (gx,gy) → window px = (480 + gx·4.5, gy·4.5).
   - THESE NUMBERS ARE 640×480-ONLY — recalibrate after any resolution change
     (self-calibrate against the memory-read mouse, as in G2).
4. **Recording video**: session must be unlocked and displaying the game
   (locked = blank frames; check `loginctl … LockedHint`). Capture:
   `ffmpeg -y -f x11grab -framerate 60 -video_size 3840x2160 -i :1.0+1080,0
    -c:v libx264 -preset ultrafast -crf 18 -pix_fmt yuv420p -t <secs> out.mp4`
   **CAPTURE AT 60 fps** for flicker hunts — the GL present runs at 60 Hz and a
   one-present dropout (16 ms) aliases invisibly into a 30 fps capture (this
   exact miss produced two false "all clean" verdicts). The user seeing an
   artifact your 30 fps analysis doesn't show = raise the framerate first.
   Surface screenshots can't catch present-level artifacts at all (they read
   the 8bpp surface, not what's displayed) — video is the ground truth.
5. **Marker-verified pairs** (engine vs ours): `echo "STRIP <phase> $(date +%s)"
   >> tagpu/gamedir/tagpu.log` before each capture, match PNG mtimes with
   `ls --time-style=+%s`. Blind alternation mislabels rows. Compose panels with
   `tools/sbs.py out.png label1 img1 label2 img2` (needs Pillow).
6. **`grep -a` on tagpu.log, always** — stray binary bytes make grep treat it
   as a binary file and silently print nothing.
7. **Sim speed**: TA's own `+`/`-` keys (up to +10, down to −9; minus stretches
   builds ~10× for capture). Keys land unreliably — verify empirically (sample
   a log value twice, 15 s apart) before trusting a speed change.
8. **Camera steering (no clicks under a locked session)**: XTEST mouse edge-
   scroll — west `xdotool mousemove 1560 800`, east `4440 1080`, north
   `3000 0`, south `3000 2159`; PARK at `3000 1080` to stop. ~1400 px/s.
   Steer by the roster log (`u### TYPE own world screen` every 300 frames) —
   roster `screen=` already includes the viewport offsets. Unit ids are NOT
   stable (alive-counter order); match by world coords.

## Closed-loop camera steering (converges in ~3 rounds)

The `units:` log line carries a fresh `eye=(x,y)` every 30 frames (~1 s) — use
it as feedback instead of the 300-frame roster/swept lines (10 s stale = blind
overshoot). Loop: read eye → scroll toward target with a duration computed from
the delta (`~1400 px/s`, clamp 0.15–3 s) → park → re-read. Break when within
200 px. See the memory log for the exact bash loop (2026-09-01 entry).

## Frame-by-frame flicker analysis (the recipe that caught the mex dropout)

1. Record 6 s per rule 4.
2. Crop the suspect unit region every frame:
   `ffmpeg -i in.mp4 -vf "crop=500:460:<winx>:<winy>" frames/f%03d.png`
3. Metric per frame — count "structure" pixels (grey/bright: `r>90 and b>90
   and |r−b|<60` on every 3rd pixel), then flag frames <70% of the average;
   assemble suspect±1 frames into a strip with Pillow and eyeball them.
4. A unit vanishing = empty-composite flicker (see r3dcache); pieces missing =
   partial render; pose jumps = temporal aliasing (one-frame lag, cosmetic).

## Launch recipe (for completeness)

```bash
export WINEPREFIX=<repo>/wineprefix DISPLAY=:1
cd <repo>/tagpu/gamedir && WINEDLLOVERRIDES="ddraw=n,b" wine TotalA.exe &
# ~15s to window; then: activate, throwaway key, Escape, space, s, Return
# (registry-preset skirmish; verify the SETUP dialog with a surface shot first)
```
Wedged (menu keys dead) = `wineserver -k` and relaunch. DLL redeploy requires
the game dead. Sandbox note: run game-dir reads/writes and wine/xdotool/ffmpeg
UNSANDBOXED — the sandbox sees a stale view of the mirror dir.
