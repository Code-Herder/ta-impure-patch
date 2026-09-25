# Binary Patches to the Original Cavedog Engine

## Summary

Total Annihilation's source was never released, so every "engine feature" added since 1998 is
a modification of the shipped 1997/98 `TotalA.exe`. The community converged on one technique,
which I was able to verify end-to-end from primary artefacts:

1. **A same-length, in-place hex edit of one string inside `TotalA.exe`'s import table** — the
   import-descriptor name `DDRAW.dll` at VA `0x004FF618` (file offset `0x000FE418`) is rewritten
   to a mod-specific 5-letter name (`TDRAW.dll`, `TAESC.dll`, `MDRAW.dll`, `ZDRAW.dll`,
   historically `spank.dll`).
2. **A proxy DLL of that name** which re-exports the real DirectDraw API (generated with
   *AheadLib*) but, in `DllMain`, installs dozens of inline hooks and constant patches into the
   running game with `VirtualProtect`, then chain-loads a real DirectDraw wrapper (cnc-ddraw).
3. Two sibling proxies do the same for the other two Cavedog-era imports: `DPLAYX.dll` →
   `TPLAYX.dll` (networking + replay recorder) and `WINMM.dll` → `TMUSI.dll` (CD-audio → MP3).

Since 2023 there is also a **hex-edit-free** variant (FunkyFr3sh's *Total Annihilation Patch
Loader*), which applies the identical byte patches at runtime so that no modified `TotalA.exe`
has to be redistributed.

`TotalA.exe` is not packed, is a plain MSVC 5.0 build, and still contains its PDB path, Cavedog
source filenames, an ~80-entry developer/cheat command table and a dozen debug command-line
switches — which is a large part of why the game proved so tractable to patch.

## Baseline: official Cavedog patches

- Cavedog shipped four official patches, ending at **3.1** (the last), which folded in most
  *Core Contingency* units plus the ARM Phalanx/Stunner and CORE Copperhead/Neutron from
  *Battle Tactics*, added the CTRL+P / CTRL+R / CTRL+W selection hotkeys, and reworked AI
  pathfinding for post-CC maps. *(Reported by the Total Annihilation Wiki via search summary;
  I could not fetch [totalannihilation.fandom.com/wiki/Patches](https://totalannihilation.fandom.com/wiki/Patches)
  or [tauniverse.com/cavedog-mirror/totala/exes.html](https://www.tauniverse.com/cavedog-mirror/totala/exes.html)
  directly — both are behind Cloudflare/blocked. Treat the per-version breakdown as unverified.)*
- **Verified from the binary itself**: the 3.1 executable's embedded build stamp is
  `Jul 30 1998` / `11:16:36`, matching a PE `TimeDateStamp` of 1998-07-30 18:22:29 UTC.
- **Verified**: the canonical "3.1c" `TotalA.exe` is exactly **1,178,624 bytes**. The community
  *Visual Patcher* refuses to run on anything else: `if br <> 1178624 then ShowMessage(... 'This
  patcher requires TA 3.1c to work!')`
  ([src/VisPatcher/main.pas](https://github.com/tanvanman/TADR/blob/master/src/VisPatcher/main.pas)).
- Official files are mirrored at `https://files.tauniverse.com/files/ta/official-files/`
  (per [PCGamingWiki](https://www.pcgamingwiki.com/wiki/Total_Annihilation)).

## TAUCP (Total Annihilation Unofficial Community Patch)

> Naming note: the community calls this the **Unofficial Patch / TA Community Patch / v3.9.02
> beta patch**. "TAUCP" on tauniverse.com is a *different* thing — the **Units Compilation
> Pack** (a unit pack, `taucp.tauniverse.com`). I could not reach tauniverse.com to confirm
> current usage; the engine patch is the 3.9.02 line described below.

**Type / Status / Source.** Three DLLs + a data archive, dropped into the TA directory next to
a hex-edited `TotalA.exe`. Actively developed — the source repository
[tanvanman/TADR](https://github.com/tanvanman/TADR) had release `dev-13d71dd` on **2026-08-31**.
Distribution:
- Ready-made drop-in bundles: [gammata/TA-Unofficial-Patch-Install](https://github.com/gammata/TA-Unofficial-Patch-Install)
  (`OTA/ProTA/ESCALATION/MAYHEM/TWILIGHT/ZERO.Dropin.Install.zip`).
- Historic installers `http://patch.tauniverse.com/ta-patch/TA_Patch_3902.exe` and
  `TA_Patch_Resources.exe` (linked from PCGamingWiki; host 403s for me).
- Per-mod DLL builds as GitHub releases: `tdraw-ota.zip`, `tdraw-prota.zip`,
  `tdraw-escalation.zip`, `tdraw-mayhem.zip`, `tdraw-tazero.zip`, `tdraw-bta.zip`.

**Authorship** (from [LICENSE](https://github.com/tanvanman/TADR/blob/master/LICENSE) and
`src/DDraw/tdraw.txt`): lineage from **SJ / Yeha** (2003, whiteboard markers + "TA Hook"),
extended by **Xpoy** (hotkeys, unicode font, megamap, weapon-ID crack, GOG music port), then
**Rime**, then **Axle1975**, **FunkyFr3sh**, **tagROCK**, **TAG_Venom**. MIT licensed.

**What it adds — concretely** (all values quoted verbatim from the shipped `Settings.ini` in
`OTA.Dropin.Install.zip` and from `EngineLimits.h`):

| Limit | TA 3.1 stock | Patch default |
|---|---|---|
| Unit limit per player | 250 | **1500** (settable 20–6553) |
| Pathfinding cycles (`AISearchMapEntries`) | 1333 | **66650** |
| Special-effects limit (`SfxLimit`) | 400 | **20480** |
| Unit model draw buffer (`X/Y_CompositeBuf`) | 600×600 | **1280×1280** (max 4096²) |
| Unique unit IDs (`UnitType`) | 512 | **16000** |
| Unique weapon IDs (`WeaponType`) | 256 | **16000** |
| Simultaneous sounds (`MixingBuffers`) | 8 | **128 / unlimited** |
| Skirmish players (`NumSkirmishPlayers`) | 4 | **10** |
| Sound mode | Mono | **3D positional** (stereo/5.1/7.1) |
| Projectiles / explosions / model effects | 300 / 300 / 100 | **3000 / 3000 / 1000** |

Plus: a **megamap** (full-screen zoomable minimap with mouse-wheel zoom, configurable icons via
`Icon/iconcfg.ini`, sensor range rings), replay recording/playback, double-click "select all of
type on screen", an expanded multiplayer sharing menu, a main-menu resolution adjuster, an
arbitrary-resolution override (`DisplayModeWidth`/`DisplayModeHeight` registry DWORDs — "all
resolutions and aspect ratios supported by your setup … as long as they are available in 8-bit
colour depth"), chat macros, and a very long bug-fix list (ghost-com bug, units exploding in
factories, `Ctrl+Z/A/B/C` truncating selections for unit IDs ≥ 512, crash on long install paths,
crash on print-screen, radar/sonar jammers jamming their own owner in >3-player games).

**How it works — mechanism, verified.**

- `TotalA.exe`'s import table lists `TDRAW.dll` (imports exactly one symbol,
  `DirectDrawCreate`), `TMUSI.dll` (imports `waveOut*`, `aux*`, `PlaySoundA`, `mciSendStringA`
  — i.e. `WINMM.dll`'s set) and `TPLAYX.dll` (ordinals #1/#2/#4 — `DPLAYX.dll`'s set). Each
  substituted name is **byte-for-byte the same length** as the original.
- `src/DDraw/ddraw.def` shows the proxy is generated by **AheadLib**: every export forwards to
  the system DLL (`AheadLib_*`) except `DirectDrawCreate`, which is intercepted.
- The runtime patcher is C++ with RTTI classes `SingleHook`, `InlineSingleHook`, `ModifyHook`,
  `TAHook`, using `VirtualProtect` (strings recoverable straight out of the shipped
  `tdraw.dll`). Example: `IncreaseAISearchMapEntriesLimit` simply overwrites 4 bytes at the
  engine's limit constant with the configured value.
- ~332 distinct hard-coded `0x004xxxxx` addresses appear across `src/DDraw/`. Sample bindings
  from `HardCodeFunctions.cpp`: `HAPI_SendBuf = 0x451BC0`, `HAPI_BroadcastMessage = 0x451DF0`,
  `FindMouseUnit = 0x48CD80`, `TAMapClick = 0x498F70`, `SendText = 0x46BC70`,
  `DrawGameScreen = 0x468CF0`, `InitInternalCommand = 0x4B7760`. The global game-state pointer
  is `*(TAdynmemStruct**)0x00511DE8`. HPI I/O is re-entered at `fopen_HPI = 0x4BB2E0`,
  `read_HPI = 0x4BB7C0`, `InitTAHPIAry = 0x41D4C0`.
- `src/DDraw/tamem.h` is a ~1,978-line reverse-engineered map of TA's structures
  (`UnitStruct`, `WeaponStruct`, `ProjectileStruct`, `UnitDefStruct`, `PlayerStruct`,
  `Object3doStruct`, GUI control structs …) with `offsetof` static-asserts.
- Repo root also carries raw IDA notes: `ta info.txt` (e.g. "`UnitDefStruct.UnitBitfields (241h)`
  / `0x2000 - Is Teleporter`", `480770 UnitScripting_Handler_Get`, `467633 - Sonar jamming!!`)
  and `ta entry point.txt` ("Point to hook; `0x49EDDD` (4 params on stack) / File offset;
  `0x9E1DD` / DLL name; `0x4FDB98`").
- Rendering is delegated: `tdraw.dll` loads `ddraw_custom.dll`, which is
  **cnc-ddraw 4.9.0.0** by FunkyFr3sh (Detours sections `.detourc`/`.detourd`; PDB
  `C:\Users\Nutzer\Source\Repos\cnc-ddraw\bin\Release\ddraw.pdb`). The bundled `ddraw.ini` has a
  `[TotalA]` profile (`lock_surfaces=true`, `singlecpu=false`, `fixwndprochook=true`) plus a
  `[Viewer]` profile for the replay viewer.
- Data changes ship as `rev31.gp3` — a plain **HAPI/HPI archive** (magic `HAPI`, version
  `0x00010000`) that the engine auto-loads because `TotalA.exe` globs `rev%s.GP3`.

**Evidence.** File hashes from `OTA.Dropin.Install.zip`: `TotalA.exe`
md5 `df08c41ea3e508a0debb6bfb16af1e5e` (1,178,624 bytes, version resource rewritten to
`FileVersion = v3.9.02 Beta` while `CompanyName` stays "Cavedog Entertainment");
`tdraw.dll` `e2cd40ae60d7e92b09afa1e97f86c927` (680,448 bytes, "TA Patch 5.0.0.0",
"Unofficial Beta Patch DDraw", PDB `C:\Users\Nutzer\source\repos\TA-ReImagined\DDraw\Output\tdraw.pdb`);
`tplayx.dll` `d38f252cb07531a2bcf8fec9e68163dc` (ProductVersion `3.9.2.416`, Delphi, source paths
`F:\TA\TADR\SVN\trunk\src\Recorder\*.pas`).

## Compatibility shims & wrappers

- **cnc-ddraw** (FunkyFr3sh, <https://github.com/FunkyFr3sh/cnc-ddraw>) is the de-facto
  DirectDraw replacement: OpenGL/Direct3D9/GDI renderers, borderless-windowed by default,
  libretro GLSL shaders (`Shaders/xbrz`, `crt`, `scanlines`, …), `Alt+Enter` toggle, cursor
  unlock hotkeys, OBS Game Capture support, mouse-sensitivity scaling. This is what makes TA
  behave on Windows 10/11 without 8-bit exclusive fullscreen.
- **Total Annihilation Patch Loader** (FunkyFr3sh, MIT,
  <https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader>) — README: *"Made to replace
  the hex edited TotalA.exe in the Total Annihilation community patch."* It is a `dplayx.dll`
  proxy (`exports.def` forwards to `tplayx.*`) whose `DllMain`:
  - verifies the stock exe with `memcmp((char*)game_exe + 0x00010000, "\x14\x68\x78\x1B\x50\x00\x8D\x4C\x24\x1B", 10)`
    and errors with *"Please install the official 3.1 patch"* otherwise;
  - reads the import-name string at `#define TA_DDRAW_DLL_STR ((char*)0x004FF618)` and the
    multiplayer version bytes at `0x0049E9C0` / `0x0049E9C9`;
  - applies `res/patches.ini` byte patches, sets a code-cave flag byte at `0x00401064`, then
    `LoadLibraryA("tdraw.dll")`.
- **Thaldren-Updates/Modular-Patch** (MIT, Oct 2025 – Dec 2025) — a from-scratch `ddraw.dll`
  that hooks the DirectDraw COM vtables and blits 8-bit palettised surfaces through the Windows
  API. Fixes "DirectX Initialization Problems on game load" and "Game Window not utilizing the
  entire screen". `DPLAYX` and `WINMM` modules are marked *work in progress*.
- **DxWnd** ships a stock `Total Annihilation.dxw` profile
  (`DxWnd/DxWnd.reloaded → build/exports/`) with `maxddinterface0=7`, `slowratio0=2`.
- **IPXWrapper** is the documented LAN fix (run `directplay-win32.reg` / `directplay-win64.reg`,
  choose "IPX Connection for DirectPlay") — [PCGamingWiki](https://www.pcgamingwiki.com/wiki/Total_Annihilation).
- **dgVoodoo2 / dxwnd for TA generally**: commonly recommended in forums; I found no
  authoritative TA-specific documentation beyond the DxWnd export above. **Unverified.**

## Other binary patches

- **TA:Escalation (TA:ESC)** — <https://taesc.tauniverse.com/>. Verified from
  `ESCALATION.Dropin.Install.zip`: it ships the *same* 680,448-byte community-patch DLL under
  the name **`taesc.dll`/`edraw.dll`**, i.e. TA:ESC's executable has the same import rename
  applied with a different target name. Its `Settings.ini` is headed *"Total Annihilation
  Escalation advanced settings v9.9.2"*, defaults `UnitLimit = 1000` ("setting higher than 1000
  WILL cause instability… All online players must have same unitlimit value"), `MP3Player = 2`,
  and adds a `UseVideoMemory` HOTFIX toggle for a GPU/Win10 crash. `tdraw.txt` records that the
  Escalation build uniquely enables a share-abuse guard (≤10 structures per 30 s), a repair-rate
  exploit fix, aircraft wrecks falling to ground, and off-map aircraft interception out to 32
  tiles. PCGamingWiki adds: minimum resolution 1024×768, campaign disabled, incompatible with
  other mods.
- **TA Demo Recorder (`tplayx.dll`)** — MIT (Rime 2015; SJ, Yeha 2003). A Delphi `dplayx.dll`
  proxy that records `.tad` replays, adds a `.`-prefixed chat-command system
  (`.commands`, `.sharelos`, `.lockon`, `.base`, `.panic`, `.report`), CRC-report commands
  (`.exereport`, `.tdreport`, `.tpreport`, `.gp3report`, `.crcreport`), stats/chat logging and a
  plugin engine. Replays are played back by `TADemo/SERVER.EXE` (Delphi).
- **TAForever** (<https://www.taforever.com/>, <https://github.com/ta-forever/gpgnet4ta>, MIT) —
  does *not* patch the exe; it tunnels TA's DirectPlay traffic through one UDP port by
  masquerading as local TA instances and rewriting DirectPlay headers (`GameAddressTranslater`).
  Its `libs/tapacket/notes/` publishes the reverse-engineered protocol (sub-packet type table,
  DirectPlay `dwUser` status-bit table, lobby status-packet field offsets `0x8C/0x91/0x97/0x9C/0xA6`).
- **`totala.ini` `[preferences] unitlimit=`** — the strings `UnitLimit`, `Preferences` and
  `DisplaymodeWidth` are present in the *stock* exe, so the ini/registry knobs pre-date the
  community patch; only the *cap* is patched.
- **"500 unit limit" file on ModDB** — exists but the page 403s for me; almost certainly a
  pre-patched exe or ini. **Unverified.**

## Stock engine defects we patch

Defects in the retail 3.1 image itself, as opposed to limits or features, that our `ddraw.dll`
patches at every attach (`tagpu_patches.c`, `patch_engine_defects`). Each patch is gated on the
stock bytes, is skipped with its reason logged when they differ, and is the identity on every input
the stock code handles correctly. The disassembly, callers and
measurements for each row are in [the engine map](exe-reverse-engineering.html), §"Engine defects
we patch". TADR fixes a different set in `TABugFix.cpp`; the engine map's "Documented patch
offsets" lists some of them.

| site | defect | trigger | reachable in stock? | our fix | can the fix change the simulation? |
|---|---|---|---|---|---|
| `0x469807..0x469825` in `DrawGameScreen 0x468CF0` (the store at `0x46981D`) | the unit sort's append never tests the row's count, so a full row runs on into the next row and a row near the end writes unit pointers past the end of SORT_UNIT_LIST (`rows·cap·4` bytes, LoadMap `0x483D45`) | more units filed in the last rows of the sweep than the list has slots left. MEASURED in our build at zoom 0.5 with `vpwide`: 150 units in the last row, 82 pointers past the end every frame | the run-on, yes (harmless, and kept). Past the end at 1× needs more than `cap` units whose feet are below the view in one row [INFERRED, not reproduced] | the append is bounded by the allocation's end: `count[row] ≤ (rows − row)·cap`, a `jmp` to a 44-byte stub | no: the list feeds only the two draw loops. Every unit whose slot is inside the list is filed exactly as stock |
| `0x421E60` `GetGridPosFeature`, reached from `0x498F4F` (and `0x40514A`) | reads `[plot+8]` with no NULL test. The plot getters return NULL for a cell off the grid, and `0x4815F0` also for an on-grid `0xFFFE` cell whose anchor offset leads off it | a pointer→world point below the scroll extent's bottom (map height less 128), where `GetTPosition`'s 128-px search answers off the map | yes, on a map shorter than the viewport plus 128 px, where the camera clamp `0x41C3C0` has no valid eye [INFERRED from `0x41C40D..0x41C431`]. No stock skirmish map is that short at retail's 1600×1200; on Lava Run at 1920×1440 the engine's terrain pass faulted first [MEASURED]. Also through the debug-level `Edge` command with a margin under 128 [INFERRED]. `0x40514A`, the resurrect order's lookup [INFERRED], is not audited | a prologue detour: a NULL plot returns the engine's own "no feature" `0xFFFF` | only where stock faults: identity for every non-NULL plot. With a stock peer in a network game, the stock peer crashes where the patched one goes on |
| `0x483FA0`, the terrain pass (called at `0x468DB0`); the fault is in the tile copy at `0x4CBE44` | indexes the tile map `main+0x1428B` from the eye and the view with no compare, and the tile set with the id it reads there, and places the window from `L − sx`. A window off the map reads before the tile map, past its end, or (a map narrower than the view) the next row's cells, and an id read outside the allocation makes a wild tile pointer. An eye in (−32, 0) reads only map cells but leaves the viewport's first `−sx` columns (or `−sy` rows) holding the last frame | an eye below 0, or a window past the map's far edge. The camera clamp `0x41C3C0` gives one where the view is larger than the scroll extent: it alternates the eye between 0 and a negative value, and the window reads off the map when the view is at least the map's width, or at least its height less 96 (at the negative eye, and at 0 as well where the view is larger than the map). With `tagpu_zoom.on` our clamp replaces it and holds the eye at 0, so the window reads off the map only where the view is larger than the map; our camera's centre range goes below 0 at every left and top edge, but only on draws our terrain pass owns, where this pass does not run. The zoom range the BAR camera replaced gave a negative eye at every left and top edge of any map above zoom 1. MEASURED faulting on the first in-play draw of Lava Run at 1920×1440 and 3840×2160 and of Coast To Coast at 3840×2160, with the shipped play set before the BAR camera, on the BAR camera at 1920×1440 (from the eye 0), and with the replaced zoom range and `terrown.off` at zoom 2 at Two Continents' NW corner | not with a skirmish map at retail's resolutions: at 1600×1200 only `example.tnt`, which is not in the skirmish list, is small enough. Our DLL's resolutions reach it: from the TNT headers, Lava Run at 1920×1440 and 2560×1440, six skirmish maps at 3440×1440 and twelve at 3840×2160 | a jump at `0x484057`: a window stock gets right (`sx ≥ 0`, `sy ≥ 0`, inside the tile map) runs the stock pass unchanged; any other is drawn by us, black where it leaves the map or where stock left a strip, each on-map cell where stock puts it, every write inside the context's clip rect, which must lie inside the surface | no: only the offscreen's pixels, and only on draws stock gets wrong. There the off-map cells and the strip are black instead of heap bytes, the next row's tiles or the last frame |
| `0x423651` in `FeatureDie 0x423550` (the pool-full `jge 0x4236F7`) | with the wreck pool empty, returns having neither started the feature's reclaim or collapse sequence nor swapped the feature, while its callers have already acted: the reclaim `0x4237D0` has paid the feature's metal and energy, and in a network game every peer is told to do the same nothing | the pool's 2048 records all held (one per 3DO corpse or heap on the map, one per GAF feature playing its sequence), then a reclaim or kill of a feature that has a sequence (Town & Country's buildings) | yes, with 2048 corpses on the map; at 1500 units a player it is the normal state of a big battle (the owner's ten-player game, 2026-09-24). TADR does not fix it | the `jge` joins `0x4236EF`, the engine's own swap for a feature with no sequence, with `x`/`y` reloaded from the arguments; the raised build also raises the pool to 8192 | **yes, by design**: the feature map, where stock leaves a paid-for feature standing. A full pool also loses the sequence's window in which further hits are ignored, so a weapon hitting every frame can chain a building through its damage stages |
| `0x423892` in the reclaim completion `0x4237D0` (`test byte [esi+0xc],1; je 0x4238AD`) | the test that refuses a GAF feature already playing a sequence reads the flags of the cell the builder aimed at, but every sequence (`FeatureDie`'s reclaim or death, a burn at `0x423468`) marks only the anchor, so a reclaim through any other cell is paid in full; when the feature has a reclaim sequence, its `FeatureDie` then returns on the marked anchor and the feature stays, reclaimable again | two or more builders ordered together onto a multi-cell feature that has a reclaim sequence, aimed at any cell but its anchor: they finish in the same moment. MEASURED on Town & Country, two commanders onto `Building15` (2900 metal): +5959 through its centre cell, +3056 through its anchor | yes, with free records and any unit limit: a group reclaim of a Town & Country building. TADR does not fix it | the test reads the anchor's flags: a stub resolves a `0xFFFE` cell to its anchor as `0x423845..0x423862` does and rejoins at `0x4238AD` or `0x423898` | **yes, by design**: a player's resources, where stock pays twice for one feature. The second builder is refused exactly as one aimed at the anchor is. The one reclaim refused that stock paid only once: through another cell of a burning or dying multi-cell feature with no reclaim sequence, which stock also refuses through its anchor |
| `0x458B87`, `0x45A470`, `0x45A7B9`, `0x459875`, `0x459CB5` (the composite scratch frame's writers: the build-state copy, the frame copy, the shadow build, the 2× bakes), the call at `0x4596D8` (the cargo merge `0x4B90A0`, which paints a carried unit into the frame with no right or bottom clip) and the rasterisers under them (`0x4C8BB0`, `0x4C8760`: 800-row span tables; `0x4C0820`, `0x4C0C70`, `0x4C1000`: 2048) | each writer sizes the one scratch frame `*(main+0x1437B)+0x10` to a unit's box with no compare against its area, and the rasterisers then append one stack entry a row for as many rows as the frame has, with no compare against their table | a unit whose box passes the frame's area (a quarter of it for the 2× bake, half for the shadow's encode), or whose frame is taller than the table under it: a modded giant. MEASURED with flat giants made from a stock solar: the build before the fix faults at `0x459EAA`; a 2× bake 1548 rows tall faulted at `0x4C8035` with its texture argument overwritten | yes: by area at 600² and at the raised 1280², by rows for a structure more than 400 rows tall at 2×. No stock unit (the largest box is 184×239). TADR raises the frame and bounds nothing | a check before each writer forms its size: a fit runs stock; otherwise the frame grows, through the engine's allocator path by hand, up to 2048 × 2048; a refused grow takes a fallback that writes nothing past the frame — the unit not drawn that frame, a 1 × 1 transparent frame, or the bake's 1× path; the merge runs only when the cargo's rectangle lies inside the frame's header box | **no**: drawing state only; identical to stock wherever the unit fits. A unit's own frame is not covered: a model more than 800 rows tall still overflows the 800-row table on the 1× path, with the fix as in stock |
| `0x42DA58`, `0x42DAC7` and `0x42BEAF..0x42BED3` (the game load's build list: the shared `TEMP UTYPE LIST`, each builder's copy at def `+0x156`, and the download appender `0x42BE30`) | the shared block and each builder's copy hold 30 `u16` type IDs, but the append runs until a `canbuild` key is missing, the copy is a fixed 15 dwords with the real count at `+0x152`, and the appender writes while the count is at most 30, so entry 30 lands past the block; the AI's pick `0x40BDB0` loops to the count | a builder with more than 30 `canbuild` and download entries. MEASURED: 40 keys added to ARMCOM read a count of 59 over 30 entries and heap bytes | no: stock's longest list is exactly 30 (`corch`, `corcsa`), with no download entries | every block holds at least `bl_room(count)` entries, 30 and then powers of two from 64, each writer growing its block through the engine's allocator before the entry that would not fit; the appender's cap is removed | as content only: a builder and the AI see the whole list, where stock read the heap past the 30th entry. A list of 30 or fewer is stock's exact block |
| `0x42DCF0`, the download menus' records (sites `0x42DD74`, `0x42DDF0`, `0x42DE12`, `0x42DF23`, `0x42DF35`) | one 0xBD-byte record per `download\*.tdf` file with room for five entries, filled with no cap, so a file of six or more writes past its record and the last file's past the block | a download file with six or more entries. MEASURED: one file of 12 entries kills the load in the C runtime's heap | no: stock's 70 files hold at most four entries | a file continues into as many records as it needs at the end of the block, which grows; the page count reads the record count | no: the builder's menu and list gain the entries the file names |
| `0x49E700`, the allocator's new handler (its text read at `0x49E7BD`, `0x49E7CD`, `0x49E7F4`) | says "Out of memory! Your hard disk may be full" when the 32-bit process has run out of address space | any failed allocation. MEASURED with 1500 types each carrying a 1 MB script | yes, with a large enough mod | the text says the game is a 32-bit program that has used the memory it can address, and how many unit types are installed; the log, the dump and the exit stay the engine's | no |
| `0x42E468`, the weapon loader `0x42E440` | the record is `main + id·0x115 + 0x2CF3` with no bound: a weapon with no `ID=` (−1) overwrites the UI and input block, one from 256 the projectile pool's header and past it; and `0x42E490` copies the section name to the record's `+0` with no bound | a weapon TDF without `ID=`, with an ID of 256 or more, or with a section name of 277 characters or more | no: stock's weapons all carry an ID, the highest 246 | a weapon whose ID is outside the build's array, or whose name does not fit its record, is skipped and logged, and a unit naming it is unarmed, as stock leaves a unit whose weapon it cannot find by name | only for such a weapon, which stock made corrupt memory |
| `0x49D280`, the weapon-fired receiver `0x49D270` (`0x0D`) | the shooter (`+0x21`) and the target (`+0x1F`) are `u16` scaled by 0x118 into the unit array with no bound, and the slot byte (`+0x23`) picks one of the shooter's three slots with none (`0x49D366`), then writes through it | a malformed or hostile message | no: every sender writes a real unit index and slot | a message naming a unit past the array, or, without the extra-weapons module, a slot past 2, is dropped; with the module, its splice bounds the slot by the unit's own count | no |
| `0x424575`, the feature-hit sender, and `0x455FB8`, the dispatch slot of its receiver `0x45544D` (`0x0F`) | the bytes `0xFD`..`0xFF` are both the IDs 253–255 and "destroyed / burned / reclaimed", so a weapon with one of those IDs that hits a feature on a peer that is not the host destroys, burns or reclaims it on the host; and a cell off the map reads through `0x481550`'s NULL at `0x4244CF` | a mod weapon with the ID 253, 254 or 255 in a network game; a malformed message | no: no stock weapon has those IDs | such a weapon's hit carries bit 11 of the cell x, which no reachable cell uses, and the receiver reads `0xFD`..`0xFF` as a sentinel only without it; the sentinels stay stock's; a cell off the map is dropped | as content only: such a weapon damages the feature, as the peer that fired it meant |
| `0x42BD29`, the menu-time loader's one call (the unit sync's keys at def `+0x13E`) | the network unit sync keys each type on `0x4B6BA0`'s checksum of its FBI, four 8-bit lanes and not a CRC, and the host keeps each key a joining peer sends once and waits for as many as the peer announced, so a joiner with two types of one key holds the battle room at SYNCHING for good | FBIs that differ in a few characters. MEASURED: 16 105 generated FBIs gave 9 991 keys and a join that never ended | no: the install's 278 unit names share no key | after a load that succeeds, each group of types sharing a key is taken in name order: the first keeps it, and each other takes a hash of its name moved past every value a type holds, the def array opened and sealed around the writes as the engine's own writers do | no: the keys depend on the names and natural keys only, not on the order the files were found in, and a type re-keyed on one peer only is reported not synced, as different content is |

## What the shipped binaries reveal

Everything below is read directly out of the shipped `TotalA.exe`.

- **Not packed.** Plain PE32, 5 sections (`.text .rdata .data .tls .rsrc`), image base
  `0x00400000`, entry `0x004E6FA0`, subsystem Windows GUI 4.0. **Linker 5.10 = Microsoft Visual
  C++ 5.0.** A ~59 KB CodeView overlay follows `.rsrc`. File-offset→VA is a constant
  `VA = file_offset + 0x400C00`.
- **Leftover developer symbols**: PDB path `C:\cavedog\wargame\Release\TotalA.pdb`; source
  filenames `c:\cavedog\wargame\{wargame,frontend,multi,endgame}.cpp`; window titles
  "Cavedog Entertainment Assert Display", "Cavedog Memory Status", "Cavedog Performance
  Monitoring".
- **Developer command-line switches**: `-debughelper -dprinton -dprintoff -dprintfile
  -enableimagehlp -disableimagehlp -enableimagehlplines -disableimagehlplines -memset -memnoset
  -memfussy -memnofussy -memfrontalign -memorystatus -performancestatus -fpufussy -fpunofussy
  -gonzo -saveresources`. Player-facing ones documented elsewhere: `-d` (windowed, mutes sound)
  and `-screenwidth`/`-screenhight` *(sic)* per PCGamingWiki.
- **Built-in console/cheat command table** at file offset `0x100744`–`0x100A44`, ~80 entries:
  `ZBuffer, Feature, TreeDeath, Senderror, SelBoxes, Search, SeaLevel, Save, ReloadAIProfiles,
  Reload, Profile, PrintWeights, Move, MemDump, Mem, Include, Edge, DPrint, DebugBreak, BurnOne,
  BurnAll, Assign, Assert, FilmSpeed, Film, ILose, IWin, Kill, Control, MakePoster, Meteor,
  NowISee, HalfShot, DoubleShot, Mapping, LOS, View, ATM, Radar, SFX, BPS, Compression,
  SetShareEnergy, SetShareMetal, ShowRanges, ShareAll, ShareRadar, ShareMapping, ShareEnergy,
  ShareMetal, ShootAll, Drop, Now, BigBrother, NoEnergy, NoMetal, Sing, NetStats, Clock, Gamma,
  ScreenChat, Logo, MusicMode, Selectable, RCache, Light, LOSType, FShadow, TShadow, SwitchAlt,
  Dither, Shadow, AntiAlias, Shading, Sound3D, CDStop, CDPlay, Give, IFace, ScrollSpeed, Contour,
  NoShake`. Nearby: `memdump.txt`, `debugdat\%s.txt`, `FORCE OUT-OF-MEMORY`, `BIGSHOT`,
  `%s\screenshots`, `Cheat Codes:`, `SkirmishCheat`. `res/patches.ini` documents an F10 debug
  mode gated behind "developers mode" at `0x00095B76`/`0x00095B79`.
- **Archive loading**: the engine globs `*.HPI`, `*.UFO`, `*.CCX` and `rev%s.GP3` (and
  `%c:\*.hpi`, `%c:\TOTALA.ID` for CD detection). The loader is Cavedog's "HAPI" library —
  `HAPIBANK`, `HapiBank::OpenBank`, `HapiBank::LoadAccount`, `HapiBank Audit File`. Networking
  is `HAPINET_*` (`HAPINET_createdplayinterface`, `HAPINET_sendpacket`, …) over DirectPlay.
  Format details (20-byte header, key transform `(((key>>6)|(key<<2))&0xFF)^0xFF`, 9-byte
  directory entries, `SQSH` chunk compression modes 0/1/2, LZ77 at `fcn.004d35f0`) are published
  in [ioma8/totala-re](https://github.com/ioma8/totala-re/blob/main/docs/HPI_AND_RESOURCES.md).
- **Engine timing** (from the same project's `ENGINE_OVERVIEW.md`): 100 ms base cadence via
  `GetTickCount` stored at `0x51FB94`; main loop `fcn.0049e830`; scheduler control block
  `0x51FBD0` with a 20-entry task queue.
- **Video** is Smacker (`smackw32.DLL`, imported by ordinal); audio is DirectSound.
- The in-game **map editor** shipped separately with *The Core Contingency*
  ([PCGamingWiki DLC table](https://www.pcgamingwiki.com/wiki/Total_Annihilation)), not as an
  exe switch.

## GOG and Steam re-releases

- **Verified**: GOG's Windows changelog for *Total Annihilation: Commander Pack* lists only
  *"Internal Update (23 July 2018) — [WIN] updated internal installer structure, no changes to
  game files"* plus two Mac-only compatibility updates (2016, 2017), per
  `https://api.gog.com/products/1207658880?expand=changelog`. So GOG has not shipped Windows
  engine changes since at least 2016.
- **Verified via a third party**: the digital re-releases *are* themselves hex-patched. From
  [winlith/TAMusic98](https://github.com/winlith/TAMusic98): *"The GOG and Steam versions of the
  game's executable are patched to load a different library, `win32.dll` instead [of
  `winmm.dll`], which intercepts the game's requests for CD Audio playback and instead plays MP3
  files from the `music` folder. This library was compiled using MSVC 2008, which unfortunately
  doesn't run on Windows 98."* Note `WINMM.dll` and `win32.dll` are the same length — the same
  trick. TAMusic98 supplies an MSVC 6.0 rebuild for real Win98.
- Corroborating artefact: the community patch's `tmusi.dll` is an MSVC 9.0 (2008) build,
  timestamped 2010-02-05, exporting the full `winmm.dll` set, with embedded PDB path
  `d:\CAVEDOG\TOTALA\win32.pdb` — i.e. the same MP3 shim, renamed `TMUSI.dll`.
- Steam app 298030 and GOG both sell the **Commander Pack** (base + *Core Contingency* +
  *Battle Tactics*). Neither includes DirectPlay; multiplayer needs the Windows DirectPlay
  optional feature and community servers, since Cavedog's/Boneyards' official servers are gone.

## Verified vs. folklore

**Verified (I inspected the artefact or read published source):** the `DDRAW.dll →
TDRAW/TAESC/MDRAW/spank` import-rename technique and its exact address `0x004FF618`; 3.1c
size 1,178,624; MSVC 5.0, unpacked, PDB and source paths; the cheat/dev command table and dev
switches; every limit in the table above; `tdraw.dll` = the community patch (MIT, sources
public, released today); `tplayx.dll` = TA Demo Recorder; `ddraw_custom.dll` = cnc-ddraw 4.9.0.0;
TA:ESC uses the identical DLL under another name; the Patch Loader's signature check and its
`patches.ini` offset list; GOG/Steam `winmm→win32.dll` music patch.

**Reported but not verified by me:** the per-version content of Cavedog's 3.0/3.1 patches;
anything stated in tauniverse.com forum threads (site blocked by Cloudflare for this session);
"the patch raises the limit to 1500 for skirmish/MP but campaign stays 250" (plausible — the
patch's own `Settings.ini` is silent on campaign); ModDB's "500 unit limit" file; general
dgVoodoo2/DxWnd recommendations for TA; any claim about Steam's build differing from GOG's.

**Folklore to be careful with:** that the patch "increases the map size cap" (I found no such
setting — what changed is the *drawing buffer* `X/Y_CompositeBuf` 600²→1280² and the megamap);
that it "adds more than 10 players" (TA already supported 10; the patch raises the *skirmish AI*
count 4→10 and adds 10-player replay watching); that TA "has no widescreen support" (it is
native — WSGF gold; the patch/cnc-ddraw add arbitrary resolutions and scaling).

## Legal / licensing status

- The community patch's own code is **MIT** (three separate grants in
  [tanvanman/TADR/LICENSE](https://github.com/tanvanman/TADR/blob/master/LICENSE) covering
  `tdraw.dll`, `tplayx.dll` and `server.exe`); cnc-ddraw, gpgnet4ta and Modular-Patch are also
  MIT/open.
- The problem is the **hex-edited `TotalA.exe`**, which is a derivative of a commercial binary
  still owned by Wargaming. The community handles this three ways:
  1. **Ship only DLLs.** `tanvanman/TADR` releases contain `tdraw-*.zip` / `tplayx-*.zip` and no
     executable; `tdraw.txt`'s install instructions say "Drop `tdraw.dll` into your TA directory"
     and require the user to own the game.
  2. **Patch at runtime instead of on disk.** FunkyFr3sh's Patch Loader exists precisely to
     "replace the hex edited TotalA.exe", and its screenshot caption reads *"Final OTA/Mod
     package could look like this (No TotalA.exe!)"*. It refuses to run unless the user's own
     official 3.1 exe matches a byte signature.
  3. **Reversible patchers.** The old *Visual Patcher* has an explicit Unpatch button that
     restores `ddraw.dll` over `spank.dll`.
- Nonetheless, `gammata/TA-Unofficial-Patch-Install`'s `OTA.Dropin.Install.zip` **does** contain
  a modified `TotalA.exe` (and `rev31.gp3`, itself derived from Cavedog data). No licence file
  accompanies it. I found **no evidence of any permission grant from Wargaming/Atari/Cavedog**;
  the distribution is tolerated rather than licensed, and the packages are explicitly framed as
  updates for a game you must buy (GOG Commander Pack is the recommended base).

## Open questions / uncertainty

- I could not read **tauniverse.com** at all (Cloudflare bot protection on `www.`,
  `patch.`, `taesc.` and `files.` subdomains) nor **web.archive.org**, **moddb.com**,
  **fandom.com** or **steamcommunity.com**. Everything sourced to those hosts here is either
  from PCGamingWiki's citation of them or from search-engine summaries.
- I do **not** have a stock 3.1 `TotalA.exe`, so I could not byte-diff it against the 3.9.02
  one. The identical file size (1,178,624) plus the preserved 1998 PE timestamp is strong
  circumstantial evidence that the on-disk edit is same-length only (import names + version
  resource strings), but a diff would settle it.
- Relationship between the 2022 "v3.9.02 / TA Patch 5.0.0.0" bundles and the 2026 `tdraw-*.zip`
  releases is not documented anywhere I could reach — I assume continuous development of the
  same codebase (the 2022 DLL's PDB says `TA-ReImagined`, which is not a public repo).
- `res/patches.ini` for OTA carries only two byte patches plus a `[Settings]` block, whereas
  `prota.ini` has 69 and `mayhem.ini` 157 — i.e. most gameplay hex patches are mod-specific,
  not part of the base patch. Worth confirming which set the shipped OTA exe actually has baked
  in.
- Whether `TMUSI.dll` is byte-identical to GOG/Steam's `win32.dll` — very likely, unconfirmed.

## Sources

- <https://www.pcgamingwiki.com/wiki/Total_Annihilation> (fetched via `api.php`)
- <https://github.com/tanvanman/TADR> — community patch + recorder source, `LICENSE`,
  `src/DDraw/tdraw.txt`, `EngineLimits.{h,cpp}`, `LimitCrack.cpp`, `HardCodeFunctions.cpp`,
  `HPIfunc.cpp`, `tamem.h`, `tafunctions.h`, `TABugFix.cpp`, `src/VisPatcher/main.pas`,
  `ta info.txt`, `ta entry point.txt`, `Docs/TANET.TXT`
- <https://github.com/gammata/TA-Unofficial-Patch-Install> — drop-in packages (binaries analysed)
- <https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader> — `dllmain.c`, `exports.def`,
  `res/patches.ini`, `res/prota.ini`, `res/mayhem.ini`
- <https://github.com/FunkyFr3sh/cnc-ddraw>
- <https://github.com/TotalA-Unofficial-Updates/Modular-Patch> (a.k.a. `Thaldren-Updates/Modular-Patch`)
- <https://github.com/ta-forever/gpgnet4ta> — `libs/tapacket/notes/*`, `README`
- <https://github.com/ioma8/totala-re> — `docs/ENGINE_OVERVIEW.md`, `docs/HPI_AND_RESOURCES.md`
- <https://github.com/winlith/TAMusic98> — GOG/Steam `win32.dll` music patch
- <https://github.com/DxWnd/DxWnd.reloaded> — `build/exports/Total Annihilation.dxw`
- <https://api.gog.com/products/1207658880?expand=changelog>
- <https://www.taforever.com/>, <https://taesc.tauniverse.com/> (referenced, not fetchable)
- <https://totalannihilation.fandom.com/wiki/Patches> (search summary only — blocked)
