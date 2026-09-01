# Windowed mode — the black-window bug and the desktop-blackout bug, both root-caused

*Two independent defects stood between us and "run TA in a normal window while a human
keeps using the PC". Both were diagnosed and fixed on 2026-09-01 (tacli track). Neither
was where the earlier notes guessed. Companion pages: `tacli-design.md` (why windowed),
`cmdline-options.md` (launch knobs), `frame-composition.md` (the render path).*

## Bug 1 — windowed renders BLACK (fixed in our fork)

**Symptom** (recorded since the G2 era as "windowed mode renders BLACK — must use
fullscreen"): with `windowed=true fullscreen=false`, the window shows nothing but our
overlay's markers. Game audio/logic run normally.

**Diagnosis chain** (each step discriminated, not guessed):

1. `tagpu_shot.trigger` surface shot = **perfect main menu** → TA draws fine; the fault
   is in cnc-ddraw's presentation, not the engine.
2. `tagpu_glshot.trigger` GL readback = black **except our overlay marker** → the GL
   context, the swap and the window are all healthy; only the game quad is missing.
3. `tagpu_overlay.off` → still black → not caused by our overlay's drawing.
4. Instrumented `gldbg_state()`/`gldbg_pixel()` in `render_ogl.c`: geometry all correct
   (`surf=640x480 render=640x480 vp=(0,0 640x480) scale=(0.625,0.938)`), texture and
   palette uploaded every frame (`uploads`/`palup` climbing, `srcsum` changing), no GL
   errors, `fbo=0`, correct texture bound — but the post-draw pixel was `(0,0,0)` and
   **`prog=0`**.

**Root cause.** `ogl_render()` binds the main program **once, before the render loop**
(`glUseProgram(g_ogl.main_program)` above `while (...)`). Our overlay pass ends every
frame with `glUseProgram(0)`. The two shader-filter branches rebind the program inside
the loop, so they are immune; the plain GL3 branch (`g_oglu_got_version3 &&
g_ogl.main_program`) never rebinds. Fullscreen/upscaled runs take a filter branch →
worked. Windowed 1:1 takes the plain branch → every frame after the first drew with no
program bound → black.

**Fix**: rebind the program per frame in the plain branch, matching what the other
branches already do (`render_ogl.c`, `gldbg_state("v3path")` site). Verified: GL
framebuffer 0.02% → **89.85% non-black**, `prog=3`, real pixel colour, with the normal
config (no `minfps=-2` crutch) and with all GPU passes (`native`/`owndraw`/`writeback`/
`scaffold`) armed.

**Lesson**: our overlay unbinding to program 0 is a state change the fork must tolerate;
any render path that relies on setup-time GL state is a latent bug of this shape.

## Bug 2 — all monitors blank for seconds at launch (fixed by prefix config)

**Symptom** (user-reported, present in fullscreen too): starting the game blanks every
physical monitor for a few seconds before the window appears — unusable when a human
shares the desktop.

**Diagnosis**: X screen dimensions never changed (sampled at 2.5 Hz through a launch) and
the X framebuffer never darkened (`x11grab` brightness flat at 243.7) → not a resolution
change and not a content blank, i.e. a **physical modeset**. `WINEDEBUG=+xrandr` showed
no mode-set calls but repeated `X11DRV_XRandR_Init` + Vulkan GPU probes per wine process.
`~/.local/share/xorg/Xorg.1.log` showed the smoking gun: **full NVIDIA output re-probe
(EDID detection on every DFP) at each launch**.

**A/B proof** (probe count = `grep -c 'DFP-3): connected'` in the Xorg log across a
cold start, wineserver killed first):

| `HKCU\Software\Wine\X11 Driver` `UseXRandR` | probes per launch |
|---|---|
| `Y` (wine default) | **70** |
| `N` | **0** |

**Fix**: set `UseXRandR=N` in each instance prefix:

```
wine reg add 'HKCU\Software\Wine\X11 Driver' /v UseXRandR /t REG_SZ /d N /f
```

Game still launches and renders identically (89.85% non-black GL framebuffer, all passes
armed). Safe here because we never want wine to change the real display mode: TA's
resolution comes from its own registry keys (`resolution.md`) and cnc-ddraw presents into
a window.

**tacli requirement**: every instance prefix gets `UseXRandR=N` at creation.

## Bug 3 — the game steals the human's mouse pointer (mitigated; root cause is wine)

**Symptom** (user-reported): starting the game snaps the real pointer to a screen or
monitor origin, which is disruptive when the human is working elsewhere.

**What was fixed in our fork.** cnc-ddraw's `mouse_lock()` warps the pointer into the
window and `ClipCursor`s it there, `mouse_unlock()` warps it again, and
`fake_ClipCursor`/`fake_SetCursorPos` re-apply the game's own requests. All of these are
now suppressed behind the trigger file **`tagpu_nowarp.on`** (helper
`tagpu_mouse_nowarp()` in `mouse.c`, cached; honoured in `mouse.c` and
`winapi_hooks.c`). tacli arms it on every instance.

**What is left.** With every warp path in the DLL swallowed *and logged*, a launch still
produced one jump to the screen origin and **zero** swallowed calls — so TA never calls
`SetCursorPos` and the warp happens inside wine's own window setup
(`WINEDEBUG=+cursor` shows no warp/grab either, only `ungrab_clipping_window` and the
game's 700-odd `GetCursorPos` polls). Not root-caused yet.

**Mitigation in tacli**: `launch` records the pointer position, and once the window is
up puts it back (`pointer_restored` in the JSON result; `--no-restore-pointer` opts out).
Measured: the pointer now leaves its place for ~0.6 s and returns, instead of staying.

## Current known-good windowed config

`ddraw.ini` (instance-private):

```
[ddraw]
renderer=openglcore
windowed=true
fullscreen=false      ; true here = borderless fullscreen, NOT a window
maintas=true
vsync=false
maxfps=60
adjmouse=true
border=true
posX=<tile x>         ; CLI assigns per instance
posY=<tile y>
[TotalA]
max_resolutions=32
lock_surfaces=true
maxgameticks=0        ; never raise: perturbs the sim
minfps=0
```

Two X windows carry the title `Total Annihilation`: the outer frame (client + decoration,
e.g. 668×546) and the 640×480 client. Match on geometry, and remember the user's Discord
and browser windows also match the *name substring* — the exact-title rule from
`ta-capture` still applies.

## Free side effect: no intro movie in windowed mode

TA refuses to play Smacker movies unless it believes it is fullscreen (string: *"You must
be in full-screen mode to play a movie"*), so a windowed launch goes **straight to the
main menu**. The `Data/1.ZRB`/`2.zrb` omission from `cmdline-options.md` remains the
belt-and-braces route for fullscreen runs.

## Debug tooling left in the tree

`render_ogl.c` now carries `gldbg_state()` (geometry/program dump) and `gldbg_pixel()`
(post-draw pixel + CPU-side surface/palette checksums + FBO/program/texture bindings),
both gated by `tagpu_gldbg.on` and rate-limited to every 64th frame. **`glGetIntegerv`
and `glReadPixels` are resolved via `GetProcAddress(opengl32)`** — the fork's
`wglGetProcAddress` returns NULL for GL 1.1 entry points under wine, and calling the NULL
pointer crashes TA (it cost one run here; the crash surfaces as the usual secondary fault
at `0x4d94e0`).

## Gotcha re-confirmed the hard way

`pkill -f TotalA.exe` matches the agent's own wrapper shell and kills the session's
command (exit 144). Use `pkill -x TotalA.exe` / `pgrep -x TotalA.exe`.
