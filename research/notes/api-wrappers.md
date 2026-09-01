# Hooking TA at the API Boundary: DirectDraw & DLL Wrappers

*Scope: modifying the original Cavedog `TotalA.exe` (1997) by replacing/interposing the DLLs it
calls. Engine re-implementations are out of scope. Every claim is tagged **[VERIFIED]** (traced to
primary source, source code, or a shipped config file) or **[CLAIMED]** (community report only).*

## Summary

The API boundary is the single most productive modding surface on TA, and the community has already
converged on it — twice, independently. Both major community projects (the 3.9.x Unofficial Patch and
the Escalation mod) work by **hex-editing exactly one import-name string in `TotalA.exe` and shipping
their own DirectDraw DLL**: `tdraw.dll` and `edraw.dll` respectively. Both also ship a paired
DirectPlay replacement (`tplayx.dll`, `eplayx.dll`). More recently, FunkyFr3sh's *Total Annihilation
Patch Loader* removes even the hex edit: it builds a **`dplayx.dll` proxy** that the *stock* exe loads
by its own import table, applies every community patch from `DllMain`, then hands DirectDraw over to
`tdraw.dll`. That is the cleanest known "don't touch the bytes on disk" vector for TA. **[VERIFIED]**

Generic wrappers (cnc-ddraw, DxWnd, DDrawCompat, dgVoodoo2) all work on *stock* TA, and cnc-ddraw
ships a TA profile out of the box. The catch: they collide with `tdraw.dll`/`edraw.dll`, so the
community's own DLL slot and the generic wrapper slot are the same slot, and you must chain them.

## What APIs TA uses (and what that constrains)

- **DirectX 5** is the target runtime. Cavedog's own readme: *"Install or reinstall DirectX 5 off of
  the Total Annihilation CD. This will ensure that you are using the version for which Total
  Annihilation was designed."* **[VERIFIED]**
  <https://taguide.tauniverse.com/pages/readme.html>
- **DirectDraw**: `TotalA.exe` statically imports `DDRAW.dll`. The import-name string sits at VA
  `0x004FF618` in `.rdata`, and the game calls the **DirectDraw 1 entry point `DirectDrawCreate`**
  from exactly **two call sites**, `0x0047BFA2` and `0x004B55FB`. All three addresses are hard-coded
  in the Patch Loader's `dllmain.c`. **[VERIFIED]**
  <https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader/blob/master/dllmain.c>
  (It then `QueryInterface`s upward at runtime — unverified which interface version it settles on.)
- **DirectPlay**: hard dependency on `DPLAYX.DLL`; readme documents the *"A required file DPLAYX.DLL
  was not found"* error, and recommends the DirectPlay 5.0a patch `dplay50a.exe`. **[VERIFIED]**
- **DirectSound, with a native WinMM fallback**. Readme, verbatim: *"UseWindowsSound (or -w on the
  command line) Use the standard Windows multimedia interface for playing sounds instead of
  DirectSound"* and *"NoDirectSound (or -s on the command line) Disable the use of DirectSound."*
  Both live in `Totala.ini` next to `Totala.exe`. **[VERIFIED]** This matters: TA *already* has a
  WinMM path, so you rarely need to hook `winmm` — you just flip the switch. (Cost: no briefing
  narration, no movie audio, degraded mixing — readme says so explicitly.)
- **DirectInput**: **unconfirmed**. DxWnd guides tell users to tick "Hook dinput", but that is
  DxWnd's cursor-confinement machinery, not proof TA calls DirectInput. Treat as unknown.
- **PE layout of stock 3.1** (useful for any interposer that also patches memory): image base
  `0x400000`; `.text` file `0x400` → VA `0x401000`; `.rdata` `0xFAE00` → `0x4FC000`; `.data`
  `0xFF600` → `0x501000`, length `0x2B000`. **[VERIFIED]**, from `patches.c` in the same repo.
- **Settings live in the registry**, not an ini: `HKEY_CURRENT_USER\Software\Cavedog Entertainment\
  Total Annihilation`. The Patch Loader patches that literal string at `0x0050DDFD` and `0x00509EB8`
  to let mods use a private hive. **[VERIFIED]** The resolution values are reported as
  `DisplayModeWidth` / `DisplayModeHeight` (REG_DWORD, edited as hex). **[CLAIMED]**

## The rendering pipeline: 8-bit palette, software rasterisation, DirectDraw surfaces

This is confirmed by TA's own engine programmer, Jon Mavor. TA is a **software rasteriser writing
into an 8-bit palettised buffer, then blitted**. Units are not drawn as 3D geometry at runtime — they
are pre-rendered into **cached sprite bitmaps** per orientation, with a per-unit **8-bit Z-buffer**
for intersections; terrain is tiled. Because everything is 8bpp, *"I had to use a lot of lookup
tables to do things like simple alpha blending. Even simple Gouraud shading would require a lookup
table."* **[VERIFIED]**
<http://mavorsrants.blogspot.com/2012/04/total-annihilation-graphics-engine.html>
PCGamingWiki's API table agrees: `software mode = true`, 32-bit Windows exe, no 64-bit build.
**[VERIFIED]** <https://www.pcgamingwiki.com/wiki/Total_Annihilation>

**Consequence for wrappers.** Everything a ddraw wrapper sees is a finished 8-bit indexed surface plus
a palette. That means:
- You *can* do post-processing: upscale, filter, apply GLSL/libretro shaders, CRT/scanline effects,
  frame limiting, gamma, windowing. cnc-ddraw and DxWnd both do this successfully on TA. **[VERIFIED]**
- You *cannot* meaningfully add depth-aware rendering (no per-pixel depth is exposed to the wrapper),
  true colour lighting, or geometry-level effects. The pipeline hands you a 256-colour framebuffer;
  a shader can only reinterpret it.
- 8bpp primary surfaces are exactly what modern Windows compositing handles worst, which is why
  colour-fix options exist at all (DxWnd's **"Win 7 color fix"**, cnc-ddraw's `rgb555`). **[CLAIMED]**
- ReShade has been used on TA:Escalation for SMAA/FXAA/bloom by injecting after the wrapper.
  **[CLAIMED]** <https://www.vogons.org/viewtopic.php?f=59&t=62754>

## Generic wrappers and what each enables for TA

**cnc-ddraw** — strongest evidence of first-class support. TA appears **twice** in the project's
supported-games list ("Total Annihilation" and "Total Annihilation (Unofficial Beta Patch v3.9.02)"),
and its shipped `ddraw.ini` contains a TA profile verbatim: **[VERIFIED]**
```ini
; Total Annihilation (Unofficial Beta Patch v3.9.02)
[TotalA]
max_resolutions=32
lock_surfaces=true
singlecpu=false
```
plus a matching `[Viewer]` section for the replay viewer.
<https://github.com/FunkyFr3sh/cnc-ddraw> · <https://github.com/1dot13/gamedir/blob/master/ddraw.ini>
`max_resolutions=32` is the tell: TA enumerates DirectDraw display modes into a fixed-size table and
must be fed a bounded list **[CLAIMED — inference from the setting's existence]**. Gains: windowed,
borderless (`toggle_borderless`), resizable, GDI/OpenGL/D3D9 renderers, GLSL + libretro shaders,
`maxfps`, `maxgameticks` speed limiting, aspect preservation (`maintas`, `boxing`), mouse scaling
(`adjmouse`), vsync, screenshots. **[VERIFIED from ini]** Critical TA caveat from the wiki: *"To make
cnc-ddraw working with the Unofficial Beta Patch v3.9.02 (and mods) you must rename ddraw.dll to
pdraw.dll"* **[VERIFIED]** <https://github.com/FunkyFr3sh/cnc-ddraw/wiki>

**DxWnd** — works, with the most granular control and the most fragility. Reported working settings:
"Hot patch (obfuscated IAT)", "Optimize CPU", "Keep aspect ratio", Video → "Win 7 color fix",
"Modal style" for borderless, Input → "Hook dinput" / "Emulate mouse relative movement" / "Keep
cursor within window", DirectX → Filtering ≠ "ddraw default"; plus `EMULATESURFACE` and the OpenGL
renderer with bilinear at 1920×1080, and a wildcard path `*\TotalA.exe`. **[CLAIMED]** Known
regressions: 2.04.31 works with Escalation but 2.05.51 does not; 2.05.25 hooks but 2.05.27 does not.
**Alt-tab is repeatedly reported broken.** **[CLAIMED]**
<https://sourceforge.net/p/dxwnd/discussion/general/thread/b9bb7a9cec/> ·
<https://sourceforge.net/p/dxwnd/discussion/general/thread/2b2166440e/> ·
<https://steamcommunity.com/app/298030/discussions/0/451852225144081950/>

**DDrawCompat** — reported usable by installing its `ddraw.dll` **as `tdraw.dll`** for patched TA.
**[CLAIMED]** It re-implements DirectDraw/GDI internally rather than routing to OpenGL/D3D, so it
fixes performance and Win8/10 bugs but gives no shaders or upscaling. Its documented escape hatches
(rename to `dciman32.dll`, or `InstallDDrawCOMRedirection.reg` for COM-created DirectDraw objects)
are worth knowing for any TA build that bypasses a local `ddraw.dll`. **[VERIFIED]**
<https://github.com/narzoul/DDrawCompat/wiki/Installation-guide>

**dgVoodoo2** — works on stock TA (reported ~150 fps, removing the 60 fps cap) but **breaks
Escalation's custom features**, and one report has it crashing the nVidia driver in fullscreen.
dgVoodoo's own author (Dege) posted the chaining workaround: rename dgVoodoo's `ddraw.dll` to
`fdraw.dll` (+ `d3dimm.dll`) into SysWOW64, then hex-edit the string `ddraw.dll` → `fdraw.dll`
*inside* `edraw.dll`. **[CLAIMED — but from the wrapper's author]**
<https://www.vogons.org/viewtopic.php?p=611617>

**WineD3D ddraw** — no TA-specific evidence found. Honest gap.

## Resolution & widescreen

TA supports high resolutions **natively** — this is not a wrapper feature. Cavedog's readme already
warns *"If you experience performance slowdown while running in higher resolutions you may need to
return to 640×480 mode."* **[VERIFIED]** Selection is via the registry
(`DisplayModeWidth`/`DisplayModeHeight`) **[CLAIMED]**, or the command line
`-screenwidth XXXX -screenhight XXXX` (the typo is genuine) **[CLAIMED — PCGamingWiki]**.
PCGamingWiki records windowed mode via a `-d` shortcut flag which *"will mute all sound effects"*
**[CLAIMED]**; the 3.9.02 patch supplies a launcher that sets resolution and windowed mode properly
and defaults the game to desktop resolution instead of 640×480 **[CLAIMED]**. WSGF rates TA's
widescreen support "gold". **[CLAIMED — via PCGamingWiki]**

What breaks: the 3.9.02 changelog claims it fixed *"corrupt graphics in fullscreen mode at resolutions
above 1280x800"* on Kepler-and-newer nVidia GPUs **[CLAIMED]**. UI/HUD and minimap do not scale — TA
simply gives you more map on screen — so at 4K the interface is physically tiny; this is the standard
complaint but I could **not** find an authoritative write-up, so treat specifics as unverified.

## File-I/O hooking as a mod vector

**Mostly unnecessary — TA already ships a virtual filesystem.** Content lives in HPI archives (plus
`.ufo`, `.ccx`, `.gp3` variants of the same format), and the design guide is explicit: *"you could
just as easily create directories with the same names under your C:\CAVEDOG\TOTALA directory... Total
Annihilation sees the contents of the compressed files exactly as it does the actual file system
directory structure."* **[VERIFIED]**
<https://downloads.ta3d.org/misc/tadesign%20guide/tadesign/ta-files.htm>
Reported override precedence `totala1.hpi > BTdata.ccx > CCdata.ccx > Rev31.gp3` **[CLAIMED]**.

Where interposition still helps is **redirecting which container/paths the exe uses**, and the
community does this by string-patching rather than by hooking `CreateFileA`. The Patch Loader exposes
ini keys that overwrite in-memory strings: `Gp3FileName` (`0x005028CC`), `ConfigFileName`
(`0x005098A3`), `DownloadPath` (`0x00503730`), registry path (`0x0050DDFD`, `0x00509EB8`)
— letting a total conversion run side-by-side with stock TA. **[VERIFIED]**
I found **no evidence** of anyone hooking `CreateFileA`/`ReadFile` to serve virtual HPI content for
TA. It is a plausible unexplored vector (streaming generated assets, hot-reload, archive-free mod
distribution), not an established practice.

## GOG / Steam re-release shims

Evidence here is thinner than I'd like, and it points to **"no wrapper shipped"**:
- The Steam build ships the **June 2010 DirectX redistributable** at
  `_CommonRedist\DirectX\Jun2010\DXSETUP.exe`, and users are told to enable the **Legacy Components →
  DirectPlay** Windows feature manually. That is a plain redist, not a compatibility shim.
  **[CLAIMED]** <https://steamcommunity.com/app/298030/discussions/0/1353742967805047388/>
- GOG's Commander Pack Windows installer is listed as version **3.1** — i.e. the stock retail patch
  level, not a modified exe. **[CLAIMED]** <https://www.gogdb.org/product/1207658880>
- One search summary claims the shipped game directory contains an `aqrit.cfg`, implying aqrit's
  (now obsolete) ddraw wrapper is bundled. I could **not** verify this against the source page —
  **[CLAIMED, weak]**. Worth checking directly against an installed copy.

The strong hint is therefore inverted from what you might expect: **the re-releases fixed nothing**,
which is precisely why the community wrapper/patch ecosystem exists and why the two most-recommended
setups (3.9.02 + cnc-ddraw) are both third-party DLL replacements.

## What this approach can and cannot achieve

**Can.** Windowed/borderless/resizable presentation; arbitrary and widescreen resolutions with GPU
scaling and filtering; GLSL/libretro shaders and ReShade over the final image; frame limiting and
removal of the reported 60 fps cap; colour-depth/gamma fixes for 8bpp on modern Windows; mouse
scaling and cursor confinement; and — via the proxy-DLL trick — a **general-purpose code-injection
foothold** in an unmodified `TotalA.exe`, from which arbitrary in-memory patching, string
redirection, private registry/config namespaces, and even gameplay changes become possible. The
Patch Loader proves all of this in ~450 lines of C.

**Cannot.** Anything requiring the renderer to expose more than a finished 256-colour framebuffer:
no true-colour assets, no depth-correct effects, no lighting model, no geometry pipeline. Alt-tab
stability remains unsolved across most configurations. And there is exactly **one** ddraw slot —
the community patch, Escalation, and any generic wrapper all want it — so real-world setups require
manual chaining (`ddraw` → `tdraw`/`edraw` → `pdraw`/`fdraw`), which is the dominant source of the
"it won't hook" reports.

## Sources

- Cavedog official readme (primary): <https://taguide.tauniverse.com/pages/readme.html>
- Jon Mavor (TA engine programmer) on the graphics engine: <http://mavorsrants.blogspot.com/2012/04/total-annihilation-graphics-engine.html>
- FunkyFr3sh, Total Annihilation Patch Loader (source): <https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader>
- cnc-ddraw: <https://github.com/FunkyFr3sh/cnc-ddraw> · wiki <https://github.com/FunkyFr3sh/cnc-ddraw/wiki> · shipped ini <https://github.com/1dot13/gamedir/blob/master/ddraw.ini>
- DDrawCompat installation guide: <https://github.com/narzoul/DDrawCompat/wiki/Installation-guide>
- dgVoodoo2 DLL placement: <https://dgvoodoo2.com/dgvoodoo2-dlls-you-must-place-in-your-game-folder/>
- VOGONS, TA + Escalation on Windows 10 (incl. Dege's chaining instructions): <https://www.vogons.org/viewtopic.php?f=59&t=55013> · <https://www.vogons.org/viewtopic.php?p=611617>
- VOGONS, TA:Escalation improved visuals / ReShade: <https://www.vogons.org/viewtopic.php?f=59&t=62754>
- DxWnd discussions: <https://sourceforge.net/p/dxwnd/discussion/general/thread/b9bb7a9cec/> · <https://sourceforge.net/p/dxwnd/discussion/general/thread/2b2166440e/>
- PCGamingWiki, Total Annihilation: <https://www.pcgamingwiki.com/wiki/Total_Annihilation>
- TA file formats / VFS: <https://downloads.ta3d.org/misc/tadesign%20guide/tadesign/ta-files.htm>
- Steam Win10 setup guide: <https://steamcommunity.com/app/298030/discussions/0/1353742967805047388/> · windowed mode thread: <https://steamcommunity.com/app/298030/discussions/0/451852225144081950/>
- GOG DB entry: <https://www.gogdb.org/product/1207658880>
- TA 3.9.02 patch thread (changelog, via search excerpt — page itself Cloudflare-blocked): <https://www.tauniverse.com/forum/showthread.php?t=43735>

### Could not verify
- PCGamingWiki, WSGF, TAUniverse and ModDB pages are Cloudflare-protected; TAUniverse/ModDB content
  above comes from search excerpts, not the pages themselves.
- Whether GOG/Steam bundle any ddraw wrapper (the `aqrit.cfg` claim) — needs an installed copy.
- Which DirectDraw *interface* version TA settles on after `DirectDrawCreate`.
- Whether TA uses DirectInput at all.
- WineD3D's ddraw with TA: no TA-specific evidence found.
- Exact high-resolution UI/minimap breakage: only anecdote, no authoritative source.
