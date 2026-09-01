# Field Notes — Our Own TA Modding Knowledge

Living lab notebook for **this project's** hands-on findings: gotchas, environment quirks, and new
engine-code mapping we verify ourselves (distinct from the community corpus documented elsewhere in
this wiki). Newest findings first within each section. Every entry dated; addresses tagged
[VERIFIED] only when we confirmed them against our own binary or a live process.

## Summary

What we learn by actually building and debugging, kept so it compounds. If it cost us an hour to
discover, it goes here so it costs the next person a minute. Community-sourced facts live in the
other pages; this page is only things **we** established on **our** machine against **our** copy.

## Debug capture — the primary review path

**Screenshots for review come from inside the game/DLL, not from the X server.** This is the
canonical way we capture gate results, and it is deliberately chosen because it is immune to the
lock-screen gotcha below.

- **Primary path — our cnc-ddraw fork's built-in screenshot.** `ss_take_screenshot()` reads the
  DirectDraw **surface memory** (`src->bmi` + `src->palette->data_rgb`, `screenshot.c`), not a GDI
  or X framebuffer grab, so it produces a correct **PNG of the actual game frame even while the
  desktop session is locked**. Default hotkey `keyscreenshot = VK_SNAPSHOT` (PrintScreen); output
  dir `screenshotdir`, default `.\Screenshots\`. [VERIFIED in source]
  - **DONE (2026-08-31): headless sentinel trigger added to our fork.** In `render_ogl.c`, right
    before `SwapBuffers`, every 8th frame we check for a `tagpu_shot.trigger` file next to the exe;
    if present we delete it and call `ss_take_screenshot(g_ddraw.primary)`. So `: > gamedir/tagpu_shot.trigger`
    from any shell produces a PNG in `gamedir/Screenshots/` of the real game surface — **verified
    working while the GNOME session was locked**. This is now the review-capture command.
  - *Still to add at G1:* capture the **final GL framebuffer** with `glReadPixels` (not just the
    source 8bpp surface) once our overlay draws on top, so review shots include the overlay.
- **Secondary path — TA's own native screenshot.** The retail engine writes `.pcx` files
  (`%s%s%s%04i.pcx`) into a `screenshots\` subdir; the `ScreenShot`/`%s\screenshots` strings are
  in the exe. Fully independent of our code — a useful cross-check. Convert with `convert x.pcx x.png`.
- **Do NOT rely on `import`/`scrot`/`maim`** for review shots: they grab the X root, which is the
  lock overlay when the session is locked, and they capture window-manager chrome otherwise.

## Environment & toolchain

- **2026-08-31 — Runtime confirmation of the DLL slots (baseline, wine `+loaddll`).** Launching
  `wine TotalA.exe` directly (NOT via `explorer /desktop`, which swallowed the child window) boots
  cleanly: exe loads at base `0x400000`, then `DDRAW.dll` loads **builtin** (wine's) — that is the
  exact slot our cnc-ddraw fork will occupy. `WIN32.dll` and `audiere.dll` load **native** from the
  game dir (the Steam music shim), and `DSOUND.dll` builtin. The static field notes above are now
  confirmed live. [VERIFIED]
- **2026-08-31 — Launch TA directly, not through `explorer /desktop`.** The virtual-desktop wrapper
  produced no game-module loads and no visible window; a plain `wine TotalA.exe` creates a real
  640×480 "Total Annihilation" window and reaches DDRAW. Use the direct form for dev.
- **2026-08-31 — GOTCHA: screenshots capture the GNOME lock screen when the session is locked.**
  The game runs fine underneath (window present in `xwininfo` tree, DDRAW loaded) but `import`/`scrot`
  grab the lock overlay instead of the game. **Visual review gates require the desktop session to be
  unlocked.** Runtime-trace gates (like G0's "is our DLL loaded") do not — they read the loaddll log.
- **2026-08-31 — Our TA is the Steam build, and it is stock 3.1 with one rename.** `TotalA.exe`
  md5 `8e74a1dffa1f5988624c52048f5b20cd`, 1,178,624 bytes. The only deviation from vanilla 3.1 is
  a same-length import rename `WINMM.dll` → `WIN32.dll` (Steam's music shim, which loads
  `audiere.dll`). `DDRAW.dll` import descriptor, the section table (`.text` @ file 0x400 / VA
  0x401000; `.data` @ 0xFF600 / VA 0x501000), the `teleporter` FBI tag (VA 0x503BA4), and
  `InitInternalCommand` (VA 0x4B7760) are all exactly where the community corpus says. **The entire
  ~470-address corpus therefore applies to our binary verbatim.** [VERIFIED]
- **2026-08-31 — Display is `:1`, not `:0`.** This box is X11 (`XDG_SESSION_TYPE=x11`) but the live
  server socket is `/tmp/.X11-unix/X1`; `:0` has no socket. Anything GUI (wine, screenshots) needs
  `DISPLAY=:1`. The agent sandbox also blocks X entirely — GUI launches must run unsandboxed.
- **2026-08-31 — Dedicated wine prefix**, not Proton, for fast iteration:
  `WINEPREFIX=…/total_annihilation_mod/wineprefix`, system wine 9.0. Proton (Steam appid 298030)
  stays the canonical late-gate target; day-to-day we drive the game-dir exe under plain wine.
- **2026-08-31 — Run TA windowed via wine's virtual desktop:**
  `wine explorer /desktop=TA,1024x768 TotalA.exe`. Keeps it in a resizable X window and off the
  real display mode — the equivalent of the borderless-window trick, for a 1997 fullscreen game.
- **2026-08-31 — G0 build recipe that works.** `make DEBUG=1 _WIN32_WINNT=0x0400` in the cnc-ddraw
  fork produces a 32-bit `ddraw.dll` with mingw i686. The Makefile already hardcodes
  `CC=i686-w64-mingw32-gcc`; `detours/*.cpp` are **not** built (guarded `#ifdef _MSC_VER`) so the
  mingw build is IAT-hook-only. `DEBUG=1` is what gives the runtime log + banner; a release build is
  silent. Deploy needs only `ddraw.dll` next to the exe — the external `Shaders/` folder is just the
  optional upscaling filter; the 8bpp→RGB palette shader is compiled into the DLL.
- **2026-08-31 — GL 4.6 on the real GPU under wine — the big enabler.** cnc-ddraw's OpenGL backend
  comes up as **GL_VERSION 4.6.0 NVIDIA 595.84, GL_RENDERER GeForce RTX 4070** through wine 9.0 — a
  thin passthrough to the native driver, not llvmpipe, not GDI. Our future GPU unit renderer has a
  full modern GL context to work in, for free. [VERIFIED from `cnc-ddraw-TotalA-1.log`]
- **2026-08-31 — Load our DLL over wine's builtin with `WINEDLLOVERRIDES="ddraw=n,b"`** (native,
  then builtin) and run from a dir containing our `ddraw.dll`. The loaddll trace then shows
  `…/DDRAW.dll: native` from our path instead of `C:\windows\system32\DDRAW.dll: builtin`.
- **2026-08-31 — Config section = exe basename → `[TotalA]`.** cnc-ddraw also honours a `[TotalA/wine]`
  section for wine-only overrides. It ships a known-good TA profile (`max_resolutions=32`,
  `lock_surfaces=true`, `singlecpu=false`). **Never set `maxgameticks>0` or a `limiter_type`** — those
  throttle TA's own sim tick, which would perturb the state our hooks read. `maxfps` only caps render.
- **2026-08-31 — Isolation via symlink mirror, not editing the Steam dir.** We run from
  `tagpu/gamedir/`, which symlinks every Steam file and adds only real `ddraw.dll` + `ddraw.ini`.
  The Steam install is never written to; `ls "$TA/ddraw.dll"` stays absent.
- **mingw-w64 i686 toolchain** (`gcc-mingw-w64-i686` 13-win32) installed 2026-08-31; provides
  `i686-w64-mingw32-gcc` + `-windres`. This is the cross-compiler for all our 32-bit DLLs.

## Preservation protocol (our safety net)

- Pristine `TotalA.exe` + md5 manifest of all 80 install files: `total_annihilation_mod/pristine/`.
- **We never modify a file in the Steam dir — only add DLLs.** `pristine/manifest.md5` is the
  tripwire (`md5sum -c`); Steam "Verify integrity of game files" is the restore path.

## Engine-code mapping (our own confirmations)

_Populated as we verify addresses live in G3–G4. Each entry: symbol, VA, how confirmed, what it does._

- **The startup DirectX-version warning path (our first patch target).** TA checks the installed
  DirectX version early in startup and, when it looks wrong (always, under wine/modern DirectX),
  shows the in-game *"installed version of Microsoft DirectX may not function properly"* dialog:
  - version check: `call 0x004B5070` at `0x004266A0` (returns 0 = "bad"); uses `dsetup.dll`'s
    `DirectXSetupGetVersion`. [VERIFIED live]
  - branch: `test %eax,%eax; jne 0x0042670A` at `0x004266A5`/`0x004266A7` — skips the warning when
    the check passes.
  - warning block `0x004266A9`–`0x00426705`: pushes the string at VA `0x004FD050`, formats it
    (`call 0x004C5740`), and shows the dialog via TA's own GUI (`call 0x004ABD90`, reading
    `*0x00511DE8 + 0x519` = desktopGUI). [VERIFIED]
- **DirectDraw caller sites, observed live (from cnc-ddraw's DEBUG trace of our own binary).**
  These are TA `.text` addresses that called into DirectDraw during boot, so they anchor the
  DDraw-facing side of the engine before G3 even starts:
  - `0x004B563C` — caller of `IDirectDraw::SetDisplayMode(640, 480, 8)` (TA sets its video mode
    here; sits right after the `DirectDrawCreate` call site `0x004B55FB` in the corpus). [VERIFIED live]
  - `0x004C650C` — caller of `IDirectDrawSurface::Lock` (per-frame primary-surface lock; this is the
    software rasteriser taking the framebuffer each frame). [VERIFIED live]
  - `0x004C659B` — caller of `IDirectDrawSurface::Unlock` (frame handed back for blit/present). [VERIFIED live]
  - The `Lock`(0x4C650C)/`Unlock`(0x4C659B) pair firing ~30×/s **is** TA's frame heartbeat — a natural
    place to hang per-frame observation, and a landmark for finding the unit-composite pass at G3.


## G1 overlay — hard-won gotchas (2026-08-31)

Getting one translucent triangle over the game cost a real debugging session. Every item below
cost time; each is a landmine for the next GL-hook we add.

- **Do NOT runtime-`LoadLibrary` a companion DLL from the render thread.** The first design loaded
  a separate `tagpu.dll` at first-present. Under wine this destabilised TA: a first-chance
  `EXCEPTION_ACCESS_VIOLATION ip=00000000` (a null call), after which **TA's own crash reporter**
  (the `EnumProcessModulesEx` + dbghelp path, code around `0x004D92xx`) ran and *itself* faulted at
  `0x004D94E0` while formatting a 16-byte-per-module field — that secondary crash is what surfaces
  in the wine log and masks the real one. Fix: **compile the overlay INTO the fork** and call it
  directly. No second module, no loader interaction, no crash. [VERIFIED]
- **`0x004D94E0` is TA's crash-report writer, not your bug.** If you ever see `c0000005 @ 0x4d94e0`,
  an *earlier* exception already happened and TA is dying while trying to report it. Re-run with
  `WINEDEBUG=+seh` and read the **first** `dispatch_exception`, not the unhandled one. [VERIFIED]
- **Under wine, `wglGetProcAddress` returns NULL for GL 1.1 entry points.** The fork loads *most*
  GL via `real_GetProcAddress(opengl32, ...)` but loads **`glGetIntegerv` via `wglGetProcAddress`**
  (opengl_utils.c ~line 198), so on wine `glGetIntegerv` is **null**. Calling it → `ip=00000000`.
  Rule for our code: resolve GL 1.0/1.1 functions (`glGetIntegerv`, `glDisable`, `glBlendFunc`,
  `glReadPixels`, …) via `GetProcAddress(GetModuleHandle("opengl32.dll"), name)`; only use
  `wglGetProcAddress` for GL 2.0+. Our `getgl()` helper tries wgl first, then falls back to the
  opengl32 export, which covers both. [VERIFIED]
- **The overlay needs no GL state save/restore at our hook site.** The callback sits immediately
  before `SwapBuffers`; nothing else touches GL afterwards, and cnc-ddraw re-binds program/VAO and
  re-sets the viewport at the top of the next frame. The overlay just cleans up after itself
  (`glUseProgram(0)`, `glBindVertexArray(0)`, `glDisable(GL_BLEND)`). Trying to save state with
  `glGetIntegerv` was itself the crash (previous point).
- **`renderer=openglcore` gives a guaranteed 3.2-core context** (verified: `GL_VERSION 3.2.0 core`
  on the 4070). Use it so our modern-GL code never lands on a compatibility context where the
  fork's legacy `glBegin` path would run instead.
- **Two capture paths, and they see different things.** cnc-ddraw's built-in screenshot reads the
  **8bpp DirectDraw surface** — it shows the game but NOT our GL overlay (the overlay is drawn after
  the surface upload). To review anything we draw in GL, capture the **composited GL framebuffer**
  with `glReadPixels` — our `tagpu_glshot.trigger` writes `tagpu_gl.ppm` (convert with
  `convert tagpu_gl.ppm out.png`). The framebuffer is the whole window (desktop-res, letterboxed),
  not the 640×480 logical surface. [VERIFIED]


## G2 state-read — findings (2026-08-31)

- **Reading live engine state works and is safe.** `*(void**)0x00511DE8` is the main game struct;
  it is valid even at the menu. We read the mouse position (`+0x2C76`, a `POINT` in game space),
  the unit array bounds (`+0x14357`/`+0x1435B`, stride `0x118`, slot 0 is a dummy), the alive mask
  (`+0x110`, alive bit `0x10000000`, skip bit `0x4000`), positions (`+0x6C/+0x70/+0x74` as signed
  shorts) and the scroll origin (`+0x1431F/+0x14323`). All reads are read-only, guarded by a
  `>0x600000` pointer sanity check so the menu (no unit array) is a safe no-op. No crash. [VERIFIED]
- **World→screen formula (binary-confirmed):** `sx = worldX - eyeX + 128`, `sy = worldY - alt/2 - eyeY + 32`.
  Our overlay converts `(sx,sy)` in 640×480 game space to NDC and draws a marker.
- **Pipeline proof via the mouse.** Drawing a marker at TA's *own* reported mouse position and
  moving the OS pointer, the marker tracks the cursor to the pixel — TA reported `game=(320,240)`
  when we moved to screen-centre. This proves struct-resolve → read → project → GPU-draw end to end.
  (The mouse is already screen-space, so it exercises everything except the scroll term; the unit
  path adds `eyeX/eyeY`, which is coded but not yet visually confirmed.)
- **GOTCHA: the locked GNOME session delivers XTEST pointer *motion* but not button *clicks*.**
  `xdotool mousemove` updates TA's in-memory mouse position (confirmed by reading it back), but
  neither `xdotool click` (XTEST) nor `click --window` (XSendEvent) reliably registers a button
  press in TA under wine while locked — a pixel-accurate click on a dialog's OK button does not
  dismiss it. `xdotool key --window` (XSendEvent keystrokes) *did* work at least once. Net: we
  cannot reliably drive TA's mouse-driven menus to start a skirmish headlessly under a locked
  session. Finishing G2's unit visual needs the GUI unlocked / a human to start a skirmish. [VERIFIED]
- **Exact click calibration (fullscreen, this display):** OS-pointer↔game-space is linear,
  `desktop_x = 1560 + game_x·4.49`, `desktop_y ≈ game_y·4.49` — derived by moving the pointer and
  reading TA's reported mouse from memory (a self-calibrating trick: the engine tells you where it
  thinks the cursor is). **Windowed mode (fullscreen=false) renders the game surface black** under
  our stack — only fullscreen composites TA's frame; use fullscreen + this calibration.


## Driving TA headless under a locked session (2026-08-31)

The locked GNOME session blocks synthetic mouse *clicks*, but two input paths DO work and together
are enough to start and play a skirmish headless — this is how G2 was finished:

- **Keyboard accelerators via `xdotool key --window` (XSendEvent).** TA's menu buttons have
  first-letter accelerators and the default button activates on `space`/`Return`. The exact path to
  a running game from the main menu: `space` (activate SINGLE) → `s` (Skirmish) → `Return` (Start).
  The skirmish is pre-configured in the registry (Player ARM vs Computer CORE, Canal Crossing), so
  no setup clicks are needed. **The first keystroke after `windowactivate` is often dropped** — send
  a throwaway key first.
- **Mouse-edge scrolling via `xdotool mousemove` (XTEST motion works).** Holding the pointer at a
  screen edge scrolls the map; nudge the position each step to keep the scroll alive. The map is
  large (units start at opposite corners), so scroll adaptively: read the overlay's `eye=(x,y)` and
  `drawn=N` log and stop when `drawn>0`. **The up-scroll edge is below the top HUD bar**, not at
  `y=0`. Left/right/bottom edges scroll as expected.
- **Self-locating via the overlay log.** Because our overlay logs `eye`, per-unit `world=` and
  `screen=` coords, we can steer the camera onto a unit numerically instead of by eye.

**G2 confirmed:** with the player commander scrolled into view, our green marker draws exactly on the
ARM Commander — live unit read + engine world→screen projection + GPU draw, over the real frame. The
world→screen formula (`sx=wx-eyeX+128`, `sy=wy-alt/2-eyeY+32`) is verified correct against live units.

## Our engine patches

Original byte-patches we author, applied at runtime from the fork's `DllMain`
(`src/tagpu_patches.c` → `tagpu_apply_patches()`), each guarded so it only fires on a byte match.
The exe on disk is never modified.

| # | What | Site | Change | Status |
|---|---|---|---|---|
| 1 | Remove the startup **DirectX version warning** dialog | VA `0x004266A7` (file `0x25AA7`) | `75`→`EB` (`jne 0x42670A` → `jmp`, always skip the warning) | ● shipped, verified: clean main menu, no dialog |

This is our **first original engine modification** — proof the runtime-patch approach works end to end
on our own binary: locate the check by its string (`0x004FD050`), find the branch, flip one byte from
`DllMain`. The dialog was also a modal blocker for menu automation, so removing it clears that too.

## Rendering-pipeline facts

- **TA has no Direct3D at all.** Units are 3D models software-rasterised into cached 8-bit sprites
  (one bitmap per orientation) with a per-unit 8-bit z-buffer, then composited into a palettised
  DirectDraw surface. "Intercept the unit draw call" = hook engine internals, not an API. [VERIFIED
  from engine programmer Jon Mavor's account; to be re-confirmed against our binary at G3.]
- **The two structs the renderer hinges on** (from TADR `tamem.h`, Ghidra-verified upstream; we
  re-verify offsets on our exe at G2): `Model3DONode` (in-memory 3DO tree — 16.16 fixed-point
  vertices, faces, GAF texture pointers, piece names) and `PrimitiveStruct` (per-unit runtime piece
  tree the COB scripts animate — `XPos/YPos/ZPos`, `XTurn/ZTurn/YTurn`, visibility, hierarchy).
- **No zoom in stock TA:** world→screen is scroll-offset + fixed orthographic. Pixel-exact matching
  in early gates is easier than expected because there's no zoom term to reproduce.
