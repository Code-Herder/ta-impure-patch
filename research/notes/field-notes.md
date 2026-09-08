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
- **2026-08-31 — Display is `:1`, not `:0`.** The reference setup is X11 (`XDG_SESSION_TYPE=x11`) but the live
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
  *2026-09-04, re-read for the Classic++ restorer:* the crash described here is the null
  `glGetIntegerv` two bullets down — the companion called a NULL GL pointer — so the module
  load itself was never isolated as the cause. **Settled the same day:** `tagpu_restore.c`
  loaded (until 2026-09-05, when the GLSL restorer replaced it) the 10 MB `onnxruntime.dll` at runtime from a worker thread of its own, and the game
  ran a 200v200 match with it in the process ([renderers](renderers.html) §2.5, roadmap
  G14a). The surviving rules: load from your own thread, never from DllMain or mid-present,
  and go through `real_LoadLibraryA` so the fork's `hook=4` `LoadLibrary` hook does not
  re-scan the new module tree.
- **Wine 9's built-in `msvcp140` is not the real one, and a missing export hangs your
  thread silently.** onnxruntime 1.21.0 and 1.22.1 (x86) call `std::_Throw_Cpp_error`, which
  Wine 9.0's `msvcp140` lacks. Standalone the process aborts with `unimplemented function
  msvcp140.dll.?_Throw_Cpp_error@std@@YAXH@Z`; **inside TA the worker thread simply never
  returned from `CreateEnv`** — no fault, no log line, the game unaffected — because the
  stub's exception passes the fork's filter (`debug.c` continues only on privileged
  instructions). Before trusting a third-party DLL in the process, run it in a standalone
  32-bit exe **under the instance's own prefix**: `WINEPREFIX=<instance>/prefix wine
  test.exe`. 1.20.1 is the last onnxruntime x86 build that runs on the built-in runtime.
  [VERIFIED 2026-09-04]
- **`tacli arm` on a not-yet-created instance** used to fail with `FileNotFoundError` on the
  gamedir, although the skill documents arming *before* the first launch. Fixed 2026-09-04:
  `cmd_arm` creates the gamedir; `mirror_gamedir()` fills in around the trigger files.
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
- **`renderer=openglcore` gives a guaranteed core context** — 3.2 until G14i, **3.3 since**
  (`render_ogl.c`; verified 2026-09-06: `tagpu.log` `shadow: GL ready (GL_VERSION 3.3.0 NVIDIA
  595.84 …)`, wine 9 on the 4070; the Classic++ shadow map's sampler objects need it). Use it
  so our modern-GL code never lands on a compatibility context where the fork's legacy
  `glBegin` path would run instead.
- **Two capture paths, and they see different things.** cnc-ddraw's built-in screenshot reads the
  **8bpp DirectDraw surface** — it shows the game but NOT our GL overlay (the overlay is drawn after
  the surface upload). To review anything we draw in GL, capture the **composited GL framebuffer**
  with `glReadPixels` — our `tagpu_glshot.trigger` writes `tagpu_gl.ppm` (convert with
  `convert tagpu_gl.ppm out.png`). The framebuffer is the whole window (desktop-res, letterboxed),
  not the 640×480 logical surface. [VERIFIED]
- **Wine keys loaded modules by PATH, so you cannot pre-empt a built-in DLL by loading yours
  first.** The trick that works on Windows — `LoadLibraryA("C:\\game\\d3d12.dll")` from our own
  thread, so a library's later `LoadLibraryA("d3d12.dll")` finds the module already loaded —
  does nothing here: the by-name load resolves to `system32\d3d12.dll`, sees a different path,
  and loads the built-in beside ours. Measured 2026-09-04 with vkd3d-proton and DirectML: both
  preloads returned valid handles and DirectML still got wine's `vkd3d`. **The only lever is
  `WINEDLLOVERRIDES`**, which is per-process env and therefore the launcher's job, not the
  DLL's — `tacli` sets `d3d12,d3d12core=n,b` ("n,b" so an instance missing the files keeps the
  built-in rather than failing the load). [VERIFIED]
- **Wine 9's built-in D3D12 (`vkd3d`) cannot host DirectML.** It creates the device on the
  RTX 4070 at feature level 11_1, then `ID3D12Device5::EnumerateMetaCommands` is a `stub!` and
  the ONNX Runtime DirectML provider append fails with `E_NOTIMPL` at
  `dml_provider_factory.cc(520)`; `CheckFeatureSupport` also answers shader model **0x51** to
  DirectML's **0x66** ask, so its DXIL shaders would not compile even past that.
  **vkd3d-proton hosts it** — upstream still ships an `x86/` pair in the release tarball, and
  **every Steam Proton carries one** at `files/lib/wine/vkd3d-proton/i386-windows`, which is
  the copy `tacli` reaches for first (measured identical, and Proton Experimental's builds the
  session faster: 1.0 s against pinned 3.0.1's 1.4–1.9 s).
  `WINEDEBUG=+dxgi,+d3d12,+vkd3d` names the stub directly; that is how this was found.
  [VERIFIED 2026-09-04]
- **A DXGI/D3D12 probe needs a live `DISPLAY` even when it draws nothing.** With no display the
  factory dies at `CreateDXGIFactory2` → `wined3d_caps_gl_ctx_create Failed to create a window`
  → `dxgi_factory_create ... hr 0x887a0004` (`DXGI_ERROR_UNSUPPORTED`), which reads like "this
  GPU cannot do D3D12" and is not. An agent shell's inherited `DISPLAY=:0` is usually the wrong
  one — check `/tmp/.X11-unix` and `xdpyinfo` (the reference setup's live session is `:1`). [VERIFIED]


## G2 state-read — findings (2026-08-31)

- **Reading live engine state works and is safe.** `*(void**)0x00511DE8` is the main game struct;
  it is valid even at the menu. We read the mouse position (`+0x2C76`, a `POINT` in **screen**
  space — `0x498DA0` is what turns it into the world point; this line said "game space" until
  2026-09-03), the unit array bounds (`+0x14357`/`+0x1435B`, stride `0x118`, slot 0 is a dummy), the alive mask
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
  `onscreen=N` log and stop when `onscreen>0`. **The up-scroll edge is below the top HUD bar**, not at
  `y=0`. Left/right/bottom edges scroll as expected.
- **Self-locating via the overlay log.** Because our overlay logs `eye`, per-unit `world=` and
  `screen=` coords, we can steer the camera onto a unit numerically instead of by eye.

**G2 confirmed:** with the player commander scrolled into view, our green marker draws exactly on the
ARM Commander — live unit read + engine world→screen projection + GPU draw, over the real frame. The
world→screen formula (`sx=wx-eyeX+128`, `sy=wy-alt/2-eyeY+32`) is verified correct against live units.

## Verification discipline — four ways a gate has lied to us

Each of these produced a confident wrong answer in a real session. They are cheap to
guard against and expensive to diagnose after the fact.

**1. An A/B diff is meaningless until the noise floor is zero.** Engine-vs-ours parity is
measured by arming the engine's own draw, capturing, flipping to ours, capturing again, and
diffing. That only works on a *frozen* scene. In the G13c fog gate a still-walking commander
moved the LOS boundary and the health bars between the two halves and produced **19,768
mismatched pixels** that were chased into the shader as a shading bug; the real figure was
**97** once everything had parked. Park the scene (`tacli eye` to pin the camera, let orders
finish), then **capture twice in the same arm state and diff those first — it must come out
zero** — and only then trust the A/B. Operational detail: ta-capture skill, "Hard rules".

**2. A one-shot `init` flag on a GL upload outlives the GL context.** `tagpu_native_glreset()`
clears the module's cached state, and any upload guarded by a `static int …Init` flag **must
be reset there too**, or after a context reset the texture object is regenerated with *no
storage* while the flag still says "uploaded". Every sample then reads 0. In G13c that turned
the entire fog grey band solid black (palette index 0) and looked exactly like a wrong remap
table. For anything small, skip the flag entirely and `glTexImage2D` every frame — the fog
shade LUT is 256 bytes and re-specs per frame for free. When a whole region of our output is
uniformly *one* value, suspect a storageless texture before suspecting the maths.

**3. Review findings must be checked against the decompile, not just against the code.** The
`code-review` skill is worth running on every gate — it found four real issues in the G13c
diff, two of them genuine bugs. But **two of the four were wrong**, because the reviewer read
our source without the engine's. It proposed a floor-mod in `fog_org()`, where the truncating
`%` deliberately mirrors the engine's own truncating division, and it flagged features being
hidden in the grey band, which is exactly what the engine's `0x4658E0` does. Both "fixes"
would have broken parity. **Where our code mirrors engine arithmetic, say so in a comment**
naming the address — that is what stops the next reader, human or agent, from correcting it.

**4. A parity diff over a fogged frame measures the fog edge, not the thing under test.**
G13b's terrain pass matches the engine's blit *exactly* — 0 differing pixels — but the
first whole-viewport diff read **0.91 %** and looked like a rendering error. All of it was
the 2–4 px band where the engine dithers its fog edge sprites and we threshold cleanly, a
deviation that was already known and deliberate. The number was identical at four unrelated
camera positions, which is the tell: **a real geometry error scales with content, a constant
across cameras is a constant feature of the frame.** Before calling a diff a defect, either
turn the confounder off (the LineOfSight setting is sticky per instance and had silently
carried over from the previous launch) or exclude its neighbourhood — dilate the difference
mask by ~8 px and re-measure what is left. On this gate that left 90.6 % of the viewport
with **zero** differing pixels, which is the number that means something.

## Our engine patches

Original byte-patches we author, applied at runtime from the fork's `DllMain`
(`src/tagpu_patches.c` → `tagpu_apply_patches()`), each guarded so it only fires on a byte match.
The exe on disk is never modified.

| # | What | Site | Change | Status |
|---|---|---|---|---|
| 1 | Remove the startup **DirectX version warning** dialog | VA `0x004266A7` (file `0x25AA7`) | `75`→`EB` (`jne 0x42670A` → `jmp`, always skip the warning) | ● shipped, verified: clean main menu, no dialog |
| 2 | **Contextual order cursors at any `Interface Type`** — restore `cursormove` over ground and `cursorreclamate` over a wreck, which the engine switches off when `Interface Type = 1` (right-mouse orders), the value `tacli` writes into every instance | VA `0x0043E50C` | `0F 84 F0 05 00 00` → `90 ×6` (drop the `je 0x43EB02` in `0x43E490`'s order-1 case) | ● shipped, verified live at Interface Type 1: ground 14 `cursormove`, wreck 11 `cursorreclamate`, own unit 15 `cursorselect`, nothing selected 19 `cursornormal`; a right-click still issues the order. Opt out with `tagpu_curs.off` |
| 2b | **Keep patch 2 cursor-only** — the left click dispatches on the cursor index patch 2 changes, so on its own patch 2 made the LEFT button issue move orders too, at Interface Type 1, from G13j until 2026-09-07 | VA `0x00499041` (inside `0x498F70`) | 27 bytes for 27: decide the contextual left click on `main+0x37EFA` and the order byte, not on `main+0x2CBE` — `cmp [eax+0x37EFA],1 / jne classic / cmp cl,1 / jne classic / jmp deselect / classic: cmp dl,0x11 / jl act / jmp done` | ● shipped, armed with patch 2 and off with the same `tagpu_curs.off`. Verified live at both interface types: at 1 a left click deselects and the right one orders, at 0 the reverse, own-unit select and the Move button unchanged, cursors still 14/11 |

| 3 | **Structure shadows are ours** — flip the `je` that sends a state-`0x20000000` unit into the blit's cached-slant-shadow branch, so under our key-filled terrain the engine no longer ALP-blends that shadow into teal | VA `0x004592C6` (path A) and `0x0045952C` (path B) of `0x459200` | `74`→`EB` on each (`je`→`jmp`, always the completed-unit branch, whose blank composite blits nothing). **Lives in `tagpu_owndraw.c`, not `tagpu_patches.c`**, and only with target `all`; installed as a pair or not at all | ● shipped (G13k), measured against the engine's shadow over engine terrain |

Patch 2 needs patch 2b, and the reasoning that said otherwise is worth keeping as a warning.
`0x43E490` does have **exactly one caller** and no address literal in the image — but that only
proves the branch governs which *index* is chosen, and the index is not a picture: `0x4992AD`
stores it in `main+0x2CBE` and the left click's action reads that byte at `0x499027` to decide
between issuing the order and deselecting. Enumerating the four sites that read `main+0x37EFA`
for left-vs-right ordering and finding them untouched proved nothing, because one of them
(`0x499046`) sits *behind* that index test. Full chain, the `cursor_ary` index → GAF table and
the measured before/after: `exe-reverse-engineering.md` §"The cursor chain — mapped by us" and
§"The in-game mouse buttons — what a click actually does".

Patch 1 is our **first original engine modification** — proof the runtime-patch approach works end to end
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
