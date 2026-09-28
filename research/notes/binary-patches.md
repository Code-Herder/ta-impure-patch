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
patches at every attach (`tagpu_patches.c`, `patch_engine_defects` and `patch_loader_defects`). Each patch is gated on the
stock bytes and is the identity on every input the stock code handles correctly. A fix that changes
the simulation fails closed: its sites join the raised limits' table, and a mismatch ends the
process through the failure report, in both builds. Any other is skipped alone, with its reason
logged. The last column ends with the class. The disassembly, callers and
measurements for each row are in [the engine map](exe-reverse-engineering.html), §"Engine defects
we patch". TADR fixes a different set in `TABugFix.cpp`; the engine map's "Documented patch
offsets" lists some of them.

| site | defect | trigger | reachable in stock? | our fix | can the fix change the simulation? |
|---|---|---|---|---|---|
| `0x469807..0x469825` in `DrawGameScreen 0x468CF0` (the store at `0x46981D`) | the unit sort's append never tests the row's count, so a full row runs on into the next row and a row near the end writes unit pointers past the end of SORT_UNIT_LIST (`rows·cap·4` bytes, LoadMap `0x483D45`) | more units filed in the last rows of the sweep than the list has slots left. MEASURED in our build at zoom 0.5 with `vpwide`: 150 units in the last row, 82 pointers past the end every frame | the run-on, yes (harmless, and kept). Past the end at 1× needs more than `cap` units whose feet are below the view in one row [INFERRED, not reproduced] | the append is bounded by the allocation's end: `count[row] ≤ (rows − row)·cap`, a `jmp` to a 44-byte stub | no: the list feeds only the two draw loops. Every unit whose slot is inside the list is filed exactly as stock. Local: skipped alone. |
| `0x421E60` `GetGridPosFeature`, reached from `0x498F4F` (and `0x40514A`) | reads `[plot+8]` with no NULL test. The plot getters return NULL for a cell off the grid, and `0x4815F0` also for an on-grid `0xFFFE` cell whose anchor offset leads off it | a pointer→world point below the scroll extent's bottom (map height less 128), where `GetTPosition`'s 128-px search answers off the map | yes, on a map shorter than the viewport plus 128 px, where the camera clamp `0x41C3C0` has no valid eye [INFERRED from `0x41C40D..0x41C431`]. No stock skirmish map is that short at retail's 1600×1200; on Lava Run at 1920×1440 the engine's terrain pass faulted first [MEASURED]. Also through the debug-level `Edge` command with a margin under 128 [INFERRED]. `0x40514A`, the resurrect order's lookup [INFERRED], is not audited | a prologue detour: a NULL plot returns the engine's own "no feature" `0xFFFF` | only where stock faults: identity for every non-NULL plot. With a stock peer in a network game, the stock peer crashes where the patched one goes on. Local: skipped alone. |
| `0x483FA0`, the terrain pass (called at `0x468DB0`); the fault is in the tile copy at `0x4CBE44` | indexes the tile map `main+0x1428B` from the eye and the view with no compare, and the tile set with the id it reads there, and places the window from `L − sx`. A window off the map reads before the tile map, past its end, or (a map narrower than the view) the next row's cells, and an id read outside the allocation makes a wild tile pointer. An eye in (−32, 0) reads only map cells but leaves the viewport's first `−sx` columns (or `−sy` rows) holding the last frame | an eye below 0, or a window past the map's far edge. The camera clamp `0x41C3C0` gives one where the view is larger than the scroll extent: it alternates the eye between 0 and a negative value, and the window reads off the map when the view is at least the map's width, or at least its height less 96 (at the negative eye, and at 0 as well where the view is larger than the map). With `tagpu_zoom.on` our clamp replaces it and holds the eye at 0, so the window reads off the map only where the view is larger than the map; our camera's centre range goes below 0 at every left and top edge, but only on draws our terrain pass owns, where this pass does not run. The zoom range the BAR camera replaced gave a negative eye at every left and top edge of any map above zoom 1. MEASURED faulting on the first in-play draw of Lava Run at 1920×1440 and 3840×2160 and of Coast To Coast at 3840×2160, with the shipped play set before the BAR camera, on the BAR camera at 1920×1440 (from the eye 0), and with the replaced zoom range and `terrown.off` at zoom 2 at Two Continents' NW corner | not with a skirmish map at retail's resolutions: at 1600×1200 only `example.tnt`, which is not in the skirmish list, is small enough. Our DLL's resolutions reach it: from the TNT headers, Lava Run at 1920×1440 and 2560×1440, six skirmish maps at 3440×1440 and twelve at 3840×2160 | a jump at `0x484057`: a window stock gets right (`sx ≥ 0`, `sy ≥ 0`, inside the tile map) runs the stock pass unchanged; any other is drawn by us, black where it leaves the map or where stock left a strip, each on-map cell where stock puts it, every write inside the context's clip rect, which must lie inside the surface | no: only the offscreen's pixels, and only on draws stock gets wrong. There the off-map cells and the strip are black instead of heap bytes, the next row's tiles or the last frame. Local: skipped alone. |
| `0x423651` in `FeatureDie 0x423550` (the pool-full `jge 0x4236F7`) | with the wreck pool empty, returns having neither started the feature's reclaim or collapse sequence nor swapped the feature, while its callers have already acted: the reclaim `0x4237D0` has paid the feature's metal and energy, and in a network game every peer is told to do the same nothing | the pool's 2048 records all held (one per 3DO corpse or heap on the map, one per GAF feature playing its sequence), then a reclaim or kill of a feature that has a sequence (Town & Country's buildings) | yes, with 2048 corpses on the map; at 1500 units a player it is the normal state of a big battle (the owner's ten-player game, 2026-09-24). TADR does not fix it | the `jge` joins `0x4236EF`, the engine's own swap for a feature with no sequence, with `x`/`y` reloaded from the arguments; the raised build also raises the pool to 8192 | **yes, by design**: the feature map, where stock leaves a paid-for feature standing. A full pool also loses the sequence's window in which further hits are ignored, so a weapon hitting every frame can chain a building through its damage stages. Fails closed. |
| `0x423892` in the reclaim completion `0x4237D0` (`test byte [esi+0xc],1; je 0x4238AD`) | the test that refuses a GAF feature already playing a sequence reads the flags of the cell the builder aimed at, but every sequence (`FeatureDie`'s reclaim or death, a burn at `0x423468`) marks only the anchor, so a reclaim through any other cell is paid in full; when the feature has a reclaim sequence, its `FeatureDie` then returns on the marked anchor and the feature stays, reclaimable again | two or more builders ordered together onto a multi-cell feature that has a reclaim sequence, aimed at any cell but its anchor: they finish in the same moment. MEASURED on Town & Country, two commanders onto `Building15` (2900 metal): +5959 through its centre cell, +3056 through its anchor | yes, with free records and any unit limit: a group reclaim of a Town & Country building. TADR does not fix it | the test reads the anchor's flags: a stub resolves a `0xFFFE` cell to its anchor as `0x423845..0x423862` does and rejoins at `0x4238AD` or `0x423898` | **yes, by design**: a player's resources, where stock pays twice for one feature. The second builder is refused exactly as one aimed at the anchor is. The one reclaim refused that stock paid only once: through another cell of a burning or dying multi-cell feature with no reclaim sequence, which stock also refuses through its anchor. Fails closed. |
| `0x43265A`, the game-load routine `0x432610`'s one call to the saved-feature restore `0x424C00` | LoadMap's border mask `0x4833B0` marks the border `0xFFFD` before a saved game's features are restored, and a spawn refuses `0xFFFD`, so a saved feature whose anchor or footprint touches a masked cell never comes back; the restore then writes an animating or 3D record's state through the refused cell's `+0x0A`, which names no record of that feature (the next row) | loading any saved game whose map has a feature on the mask's cells (the last two columns, the top and bottom rows the projection pushes off the view, low cells of a lava world). MEASURED on Two Continents: 51 of 4 893 features lost by a save and its load | yes, on every such map, in every saved game. TADR does not fix it | the call goes to a wrapper that, when the grid holds no feature, empties the mask's cells, runs the restore unchanged, marks every one of them that is then empty or a footprint cell `0xFFFD` again, and refreshes the pathing maps around each with `0x440A40`, the engine's own refresh for a grid change; it runs only while no feature and no unit is on the grid | **yes, by design**: the features a save held come back, and they block building and movement, burn and pay out on reclaim as they did before the save. Nothing for multiplayer: the retail game has no saved multiplayer game. The options menu greys its save and load buttons in a network game (`0x460CF7`, `0x460D37`), the console's `Save` is a debug-level command, and a load from the main menu takes its game type from the save. Fails closed. |
| `0x4250C0` and `0x425185` in the saved-feature restore `0x424C00` (the writes of an `Animating Features` record's state and a `3D Features` record's `+6`) | the saved state goes into the wreck record the cell's `+0x0A` names without a test that the feature came back and took one; when it did not, `+0x0A` is the spawn's 0, a freed record's index or a word no spawn wrote, so the write lands on record 0 — another feature's — or, through a stale word, up to 3 MB past the pool's base | a saved animating or 3D feature whose spawn is refused (a void cell, a footprint past the map's edge, an indestructible occupant), an animating one whose sequence finds no record (a save from the raised build, 8 192 records, loaded with stock limits, 2 048), or a feature of the other kind on the cell (the content changed between the save and the load). MEASURED on Two Continents with the free list emptied for the restore: a refused wreck wrote record 0's `+0x26` | yes, whenever a save is loaded into a smaller pool or with other content, or holds a feature on a void cell or with a footprint past the map's edge. TADR does not fix it | each block runs only when the cell is an anchor of the record's own kind that owns a record — for an animating record GAF with its sequence mark set, for a 3D record 3DO, the rule the save writer sorts its records by — and its `+0x0A` is below the pool's allocated count; otherwise the loop moves on to its next record | **yes, by design**: a feature's saved state no longer lands in another feature's record. A record whose feature did not come back as saved loses its state, which has nowhere to go. Nothing for multiplayer: the retail game has no saved multiplayer game. Fails closed. |
| `0x458B87`, `0x45A470`, `0x45A7B9`, `0x459875`, `0x459CB5` (the composite scratch frame's writers: the build-state copy, the frame copy, the shadow build, the 2× bakes), the call at `0x4596D8` (the cargo merge `0x4B90A0`, which paints a carried unit into the frame with no right or bottom clip) and the rasterisers under them (`0x4C8BB0`, `0x4C8760`: 800-row span tables; `0x4C0820`, `0x4C0C70`, `0x4C1000`: 2048) | each writer sizes the one scratch frame `*(main+0x1437B)+0x10` to a unit's box with no compare against its area, and the rasterisers then append one stack entry a row for as many rows as the frame has, with no compare against their table | a unit whose box passes the frame's area (a quarter of it for the 2× bake, half for the shadow's encode), or whose frame is taller than the table under it: a modded giant. MEASURED with flat giants made from a stock solar: the build before the fix faults at `0x459EAA`; a 2× bake 1548 rows tall faulted at `0x4C8035` with its texture argument overwritten | yes: by area at 600² and at the raised 1280², by rows for a structure more than 400 rows tall at 2×. No stock unit (the largest box is 184×239). TADR raises the frame and bounds nothing | a check before each writer forms its size: a fit runs stock; otherwise the frame grows, through the engine's allocator path by hand, up to 2048 × 2048; a refused grow takes a fallback that writes nothing past the frame — the unit not drawn that frame, a 1 × 1 transparent frame, or the bake's 1× path; the merge runs only when the cargo's rectangle lies inside the frame's header box | **no**: drawing state only; identical to stock wherever the unit fits. A unit's own frame is not covered: a model more than 800 rows tall still overflows the 800-row table on the 1× path, with the fix as in stock. Local: skipped alone. |
| `0x42DA58`, `0x42DAC7` and `0x42BEAF..0x42BED3` (the game load's build list: the shared `TEMP UTYPE LIST`, each builder's copy at def `+0x156`, and the download appender `0x42BE30`) | the shared block and each builder's copy hold 30 `u16` type IDs, but the append runs until a `canbuild` key is missing, the copy is a fixed 15 dwords with the real count at `+0x152`, and the appender writes while the count is at most 30, so entry 30 lands past the block; the AI's pick `0x40BDB0` loops to the count | a builder with more than 30 `canbuild` and download entries. MEASURED: 40 keys added to ARMCOM read a count of 59 over 30 entries and heap bytes | no: stock's longest list is exactly 30 (`corch`, `corcsa`), with no download entries | every block holds at least `bl_room(count)` entries, 30 and then powers of two from 64, each writer growing its block through the engine's allocator before the entry that would not fit; the appender's cap is removed | as content only: a builder and the AI see the whole list, where stock read the heap past the 30th entry. A list of 30 or fewer is stock's exact block. Fails closed. |
| `0x42DCF0`, the download menus' records (sites `0x42DD74`, `0x42DDF0`, `0x42DE12`, `0x42DF23`, `0x42DF35`) | one 0xBD-byte record per `download\*.tdf` file with room for five entries, filled with no cap, so a file of six or more writes past its record and the last file's past the block | a download file with six or more entries. MEASURED: one file of 12 entries kills the load in the C runtime's heap | no: stock's 70 files hold at most four entries | a file continues into as many records as it needs at the end of the block, which grows; the page count reads the record count | no: the builder's menu and list gain the entries the file names. Fails closed. |
| `0x49E700`, the allocator's new handler (its text read at `0x49E7BD`, `0x49E7CD`, `0x49E7F4`) | says "Out of memory! Your hard disk may be full" when the 32-bit process has run out of address space | any failed allocation. MEASURED with 1500 types each carrying a 1 MB script | yes, with a large enough mod | the text says the game is a 32-bit program that has used the memory it can address, and how many unit types are installed; the log, the dump and the exit stay the engine's | no. Local: skipped alone. |
| `0x42E468`, the weapon loader `0x42E440` | the record is `main + id·0x115 + 0x2CF3` with no bound: a weapon with no `ID=` (−1) overwrites the UI and input block, one from 256 the projectile pool's header and past it; and `0x42E490` copies the section name to the record's `+0` with no bound | a weapon TDF without `ID=`, with an ID of 256 or more, or with a section name of 277 characters or more | no: stock's weapons all carry an ID, the highest 246 | a weapon whose ID is outside the build's array, or whose name does not fit its record, is skipped and logged, and a unit naming it is unarmed, as stock leaves a unit whose weapon it cannot find by name | only for such a weapon, which stock made corrupt memory. Fails closed. |
| `0x49D280`, the weapon-fired receiver `0x49D270` (`0x0D`) | the shooter (`+0x21`) and the target (`+0x1F`) are `u16` scaled by 0x118 into the unit array with no bound, and the slot byte (`+0x23`) picks one of the shooter's three slots with none (`0x49D366`), then writes through it | a malformed or hostile message | no: every sender writes a real unit index and slot | a message naming a unit past the array, or, without the extra-weapons module, a slot past 2, is dropped; with the module, its splice bounds the slot by the unit's own count. **Landing B3** adds: a message whose shooter slot's weapon is not `&Weapons[id]` is dropped and counted — a diverged peer, where stock divides by the local slot weapon's `+0x68` (`0x49CE62..0x49CE6A`), faulting #DE only when it is 0 and otherwise building a projectile from the packet weapon's branch and the local weapon's velocity | no. Fails closed: the ID bounds, and the B3 diverged-drop with them, which evidence §8 classes simulation (only when diverged; never between consistent peers, since the sender fills the packet from that slot). |
| `0x4861F7` (`0x09`), `0x4553FE` (`0x0A` attach, at the dispatcher's case), `0x4866E5`+`0x486753` (`0x0C` destroy + killer), `0x489CED` (`0x0B` damage), and the `0x2C` stat/move receiver `0x48B920` at `0x48B960` (length + block), `0x48B985` (delta, length), `0x48B9AD` (type + move class), `0x48BA05` (mover after create), `0x48BA5E` (the round-robin flag's length), `0x48B40E` (round-robin type), `0x48B49C` (round-robin model object), `0x48BA9F` (unsigned remainder), `0x44E0DE` (a move payload's target) and `0x48B574` (the round robin's carrier), the copy at `0x48B92B`, with the length taken in the receive at `0x453595`/`0x45361F` (which also keeps the pump's message pointer on the buffer), and the splitter's length at `0x463939`/`0x463B33` — landing B3 | the network receivers index the unit array (begin `main+0x14357`, last `main+0x1435B`, stride `0x118`) by a `u16` off the wire with no bound (index 0 faults; index past the array points into foreign memory), the type indexes the def table `main+0x1439B + type·0x249` unbounded, a `0x09` may name a slot outside its sender's block, the `0x0A` attach `0x48AB70` scales its child and parent ids unbounded (`0x48AB8A`, `0x48ABAF`), and the `0x2C` receiver also trusts a signed delta, a missing move class (`def+0x22F`, 13 field faults at `0x48BA07`), a NULL mover after a refused create (`0x48BA05`), a NULL model object after one (`0x48B4A6`), the signed `idiv` remainder at `0x48BAAB`, the player's block pointer `[player+0x67]`, two unit references nested in its stream (a `0x4FD9E0` move payload's target, `0x44E0D0` → `0x489690`; the round robin's 15-bit carrier, `0x48B56B` → `0x48AB70`), and a bit reader with no end: the message's own size is read and discarded (`0x48B944`); the splitter `0x463790` loops for good on a message of length 0, and the pump `0x453D40` keeps a message pointer the receive's growth can free | a malformed or foreign message | no: locally every index is a live unit's own `+0xA8` (≥ 1), and every wire create comes from its unit's own player (18 011 of 18 011 measured) | each value is bounded by a pure predicate before use; a bad `0x09`/`0x0B`/`0x0C`, or a `0x09` outside its sender's block, is dropped through the receiver's own exit, and a `0x0A` whose child is not a slot or whose parent is neither 0 nor a slot through the pump's back edge `0x455F50`; the `0x2C` is parsed from a per-thread copy followed by 48 zero bytes (229 bits, the most read past the last check, by disassembly), its reader bounded by the message's length (the receive's, and the size field) before each read the stubs precede, and a misframed `0x2C` is pointed at a zero dword and sent to the engine's end-of-list `0x48BA28`; a length that cannot advance the splitter ends its walk through `0x463949`/`0x463B91`; the pump's pointer is re-pointed at the buffer after each receive; the block pointer is checked to be a valid slot run `begin+(1+k·N)·0x118`, `k` the block's rank; a nested reference past the array becomes 0, the engine's own no unit at both sites. `tagpu_wirecheck.on` checks the C predicates (22/22 OK measured; 33 cases since the nested references and the `0x0A`, not yet run in the game); the stubs rest on the disassembly | no: stock-exact for every well-formed message. Local: skipped alone. |
| `0x486036` in the allocator `0x485F50` (first-free), `0x486DC1` in the destructor `0x4866D0`, `0x48634F` in `CreateFromNetwork 0x4861D0`, `0x4854A0` (the unit array's allocation), `0x4653DE` (after the Deathmatch respawn's create `0x4653D9`), the sends `0x4560AE` (`0x09`) and `0x489CB9`/`0x489CCD` (`0x0B`), and the dispatch slots `0x455F90` (`0x05`), `0x455FA0` (`0x09`), `0x455FA8` (`0x0B`) — landing B4 | a hit is computed on the attacker's peer and sent as a `0x0B` that names the victim by slot alone; the owner's allocator is first-free, so the slot a death frees is taken by its next create at once, and a hit still in flight lands on the new unit — on the owner, which is authoritative for its death, and on every bystander | any network game with deaths and new units: a factory's next unit, a rebuilt tower. MEASURED on the previous build with every hit delayed 30 ticks (the test lever): hits landed on units younger than the delay on the owner and on a bystander | yes, in ordinary play; the delay only widens the window a real link's latency opens | the `0x09` and `0x0B` travel inside tagged `0x05` messages that carry the unit's incarnation (the owner's GameTime at the create, rising per slot; a lower bound for a copy the `0x2C` made); the owner applies a hit iff its unit's birth ≤ the stamp, a bystander refuses only a provably stale one; a bare `0x09`/`0x0B` is dropped; first-free takes an unheld free slot, else the one freed longest ago if in an earlier tick, never one freed this tick; the Deathmatch respawn, which uses its unit untested, takes the block's end on NULL and writes nothing, so its own countdown fires again six passes (about 180 ticks) later | yes, by design: a stale hit is not applied, and a create can take a later slot than stock's. A peer without it applies hits this build refuses and cannot read its creates. Simulation: fail closed, both builds. |
| `0x497F5E` in the load state `0x497F40` (its first call), `0x49842F` (the in-play entry's call of the frame function), `0x45477F` (the dispatcher's refusal in state 5), `0x48BA00` (the `0x2C` dirty entry's `call 0x4861D0`), and the hold inside landing B4's `0x05` receiver (`0x455F90`) — landing B5 | the dispatcher passes a unit create (`0x09`, since B4 carried in a `0x4A`) only in state 6 (`0x45473F`, `0x512BC0`), and each peer creates its commander from its loader while still in state 5; a peer whose load ends later refuses the others' commanders, which then exist on it only when the round robin re-creates them, and a commander that moves first comes back through a dirty `0x2C` entry whose create takes the slot's own stale position, `(0,0,0)` in a fresh array | any network game whose peers do not finish loading together. MEASURED on the previous build, two peers: the joiner lacked the host's commander for 48–50 s at the 1500-unit limit and 16–18 s at 500; a commander ordered to move at once appeared on the joiner at the map's corner, `(1,−2)` and `(2,−3)`, and walked from there until the round robin moved it | yes: stock's round robin takes 16.7 s at 500; our 1500 raise triples the window | the refused create is held per sender in arrival order (ten × 64, under a lock, the overflow counted and left to the round robin), emptied at the load's start, and replayed before the first tick through B4's receiver past its state test and `CreateFromNetwork`, only from a sender that passes the dispatcher's own sender test under the same DirectPlay id and into a slot that is empty or older by B4's stamps; a create refused in the catch-up ticks that follow is made at once, in the pump it arrives in; a `0x0C` refused meanwhile cancels its unit's held create, or marks a copy made before state 6 dying as the engine's ghost sweep does; the dirty create takes the position its entry's move payload carries (ground: path point 0, the last node the unit reached; air: selector 2's x, y, z), on the map only, read from B3's copy of the message within its length — so B5 requires B3 armed, and the table is refused (the process ends through the report) when it is not; B3 stays local for its own purpose | yes, by design: a commander exists on every peer from the pump its create arrives in — before the first tick if the load refused it, in the catch-up tick it arrives in otherwise — a unit destroyed during the load is not made, and a dirty create lands where its payload says (a unit well along a straight move at its origin, until the round robin moves it). Simulation: fail closed, both builds. |
| `0x48666D` (`Send_UnitDeath`'s one send of the `0x0C`), the `0x4C` branch of landing B4's `0x05` receiver (`0x455F90`), the `0x0C` dispatch slot `0x455FAC` (and the case `0x45541C..0x455427` compared), the send's entry `0x451DF0`, the creates of the capture `0x488700`, the placed units `0x488462` and the resurrection `0x405104` with their flushes `0x488743`, `0x488791`, `0x4884AC`, `0x405119`, `0x405155` and `0x405164`, and the `0x12` case's `0x4555BA` — landing B8 | every peer runs the destructor `0x4866D0` for every death, and four of its decisions read the victim's `+0x104` (the build fraction left) on that peer's own copy: the unit's kill count `+0xB8` (`0x4869A7`), its player's Kills (`0x4868D3`), a reclaim's credit (`0x486CBD`) and the death explosion (`0x486D2F`). `CreateFromNetwork` makes a copy unfinished at HP 0 whatever its owner's unit is, and the copy takes the owner's values only when the owner's round robin reaches its slot, up to N owner ticks later, or from a `0x12`, which the owner sends only for a type with no move class and a peer takes only for a type with a build list. The `0x12` case also indexes the unit array by its two slots unbounded (`0x4555BA`) | a unit killed before the owner's round robin reaches its copy: a factory's new unit, a rebuilt tower. MEASURED on the previous build, three peers: four ARMCK killed within ~1 s of their create added 0 / 4 / 0 kills on the host, their owner and a bystander, and the remote copies read 1.0 for 26–37 s | yes, in ordinary play: stock's round robin takes 16.7 s at 500; our 1500 raise triples the window | the `0x0C` travels in a tagged 65-byte `0x05` (`0x4C`) carrying the victim's `+0x104` as the owner reads it, and every other peer writes that value (a number in [0, 1], into a live copy) before running the destructor on the record as the `0x0C` case does; a death that the `0x0C`'s row of the dispatcher's state table (`0x512BC0`) refuses during the load goes to landing B5's refusal, whose dying mark in the catch-up ticks writes the same value first, and a bare `0x0C` is dropped. B4's create (`0x4A`) also carries the unit's `+0x104` and HP, which `CreateFromNetwork`'s exit writes into the copy as the round robin does. The three engine callers that write a new unit's HP or fraction after the create, and `tacli`'s scenario applier, hold their thread's sends at `0x451DF0` from the create to a flush after their writes, which reads the unit again and sends the queue in order: at most the 4 messages and 92 bytes one create's direct calls send, by disassembly, one entry for each of the two threads that create; a message past the bound (a unit script's) sends the queue at once, in order, its create without a state, so that copy starts as stock's. The `0x12`'s two slots are bounded as landing B3 bounds the receivers | **yes, by design**: every peer's copy starts from its owner's values, and every peer that has a copy counts a kill, credits a reclaim and plays a death on the owner's value; a unit created and destroyed while a peer is still loading never exists there, and that peer counts none of its kill. A peer without it counts on its own copy and cannot read the deaths. The increments agree; a total set on one peer (a saved game, a scenario's `kills`) does not, and the saved-game restore `0x487080` is not held. Simulation: fail closed, both builds; the `0x12` bound is local. |
| `0x424575`, the feature-hit sender, and `0x455FB8`, the dispatch slot of its receiver `0x45544D` (`0x0F`) | the bytes `0xFD`..`0xFF` are both the IDs 253–255 and "destroyed / burned / reclaimed", so a weapon with one of those IDs that hits a feature on a peer that is not the host destroys, burns or reclaims it on the host; and a cell off the map reads through `0x481550`'s NULL at `0x4244CF` | a mod weapon with the ID 253, 254 or 255 in a network game; a malformed message | no: no stock weapon has those IDs | such a weapon's hit carries bit 11 of the cell x, which no reachable cell uses, and the receiver reads `0xFD`..`0xFF` as a sentinel only without it; the sentinels stay stock's; a cell off the map is dropped | as content only: such a weapon damages the feature, as the peer that fired it meant. Fails closed. |
| `0x42BD29`, the menu-time loader's one call (the unit sync's keys at def `+0x13E`) | the network unit sync keys each type on `0x4B6BA0`'s checksum of its FBI, four 8-bit lanes and not a CRC, and the host keeps each key a joining peer sends once and waits for as many as the peer announced, so a joiner with two types of one key holds the battle room at SYNCHING for good | FBIs that differ in a few characters. MEASURED: 16 105 generated FBIs gave 9 991 keys and a join that never ended | no: the install's 278 unit names share no key | after a load that succeeds, each group of types sharing a key is taken in name order: the first keeps it, and each other takes a hash of its name moved past every value a type holds, the def array opened and sealed around the writes as the engine's own writers do | no: the keys depend on the names and natural keys only, not on the order the files were found in, and a type re-keyed on one peer only is reported not synced, as different content is. Fails closed. |
| `0x49A0A9`, `0x49A109` (the two calls of area damage `0x49A120`), `0x49A262`, `0x49A5CE` (its unit and feature lists) | the lists hold 20 units and 64 feature anchors, and the damage runs whether a victim was recorded or not, so a victim found past them is hit once per cell of it in the blast (a feature once per cell whose own position is inside the radius) | any explosion reaching more than 20 units or 64 features: a commander's blast or a nuke on a dense base | yes, in ordinary play. MEASURED: 16 of a ring of 36 CORFLAK lost exactly 4× their mirror mates; 30 of the 32 wrecks past the 64th destroyed | a wrapper on both calls holds, as a local on the calling thread's stack, a frame of two hash sets (unit slots, anchor ordinals) with room for 32 keys each before they grow, and the list blocks find the innermost frame through a TLS slot; each index bounded first; every victim hit once | **yes, by design**. Fails closed. |
| `0x49CF18` in the ballistic fire `0x49CDE0`; `0x42F314`, `0x42F32E` (the loader's calls of `0x49E010`) | a burnblow shot divides the distance by `v·cos(pitch)`, 0 within 0.35° of vertical (`0x49CF19`), and every ballistic shot by `w+0x68`, 0 for a weapon with `weaponvelocity` 0 (`0x49CE6A`) | flak aimed nearly straight up, on the firing peer and every peer the `0x0D` reaches; a modded ballistic weapon with velocity 0 | not reproduced: stock acquisition never aimed above 29.6° | a zero divisor takes `weapontimer`, as a non-burnblow shot does; a zero velocity becomes 1 at load, logged | only where stock faults. Local: skipped alone. |
| `0x47CC8B`, `0x47CCA3`, `0x47CCA9` in the grid stamp `0x47CC30` | `X + fw ≥ W` / `Z + fh ≥ H` park a unit whose footprint ends on the last column or row in the off-map bucket, where nothing can hit it | an aircraft over the east or south edge | yes. MEASURED: an ARMATLAS on the last three columns untouched for 40 s in a flak's range | `jge` → `jg`, and the sort bucket's index kept as stock's wherever it lands inside the array and clamped into the grid only where it does not | **yes, by design**. Fails closed. |
| `0x49A664`, `0x49A415` in area damage `0x49A120`; `0x47CF98` in the grid stamp `0x47CC30`; `0x4954ED` in the sim step `0x495490` | area damage finds victims only in each cell's two unit slots, and a contested slot B keeps one aircraft, so an aircraft no in-rect slot names takes no splash | aircraft stacked over one point: ten ARMATLAS ordered there, up to seven held no cell | yes. MEASURED: a CORFLAK burst took the four holders to 5–11 HP and left the five holding none at 150 | after stock's walk, the aircraft it missed are handed to its own per-victim code one at a time, from a pool of airborne slots rebuilt at the unit tick's call and added to by the stamp's airborne path, each re-tested at hand-over by slot B's rule (bit 29 in the mask, so a bit-29 unit is excluded), in the grid (a real bucket) and not dying (bit 14) | **yes, by design**: stacked aircraft now take splash. Fails closed. |
| `0x465B6A`, `0x465C04`, `0x465CA2`, `0x465D46` in `UnitInPlayerLOS 0x465AC0` (one read for each of the four points of the box it tests), `0x465DA9` and `0x408095` in `PositionInPlayerMapped 0x408090`, `0x407F74`, the same read inlined in `0x408090`'s AI caller `0x407E90`, the order resolver `0x43F0E0`'s six (`0x43F5D1`, `0x43F631..0x43F654`, `0x43FC05`, `0x43FD1A`, `0x43FF85`, `0x4400AA`) and the view player's map build `0x467440`'s two (`0x46778A`, `0x4677D0`) | the row is `(z − y/2) >> 5` under an unsigned bound, so a point high up near the north edge (or underwater near the south one) is off the grid: a unit is invisible to every other player, the order resolver treats its target cell as unmapped, and the view player's map never marks it seen for the acquisition. the census finds 46 copies of the read, the sight emitter's among them (next row): 46 of the 57 compares in `.text` that read a grid's height in memory. It does not follow a grid pointer spilled to the stack or a height recomputed from the plot's rows; the second pass over those forms found only the sight grid's builder, which projects terrain into the grid in sheared space and is not the defect. The engine map's census names each | any line-of-sight mode: True reads the LOS grid, Permanent and Circular the shared mapped grid `main+0x14273` through `0x408090` and the inline mapped copies, with the same shear; an aircraft at cruise altitude within about 6 tiles of the north edge | yes. MEASURED: an ARMATLAS at z 40 hovered 7 s at full HP in a human flak's range | the point's own row when the sheared one is off the grid; beyond the edge stays unseen. Each stub counts the own row per function | **yes, by design**. Fails closed. |
| `0x482615..0x48261E` in the sight emitter `0x4825B0` (the rest of `0x4825D4..0x48266A` compared: its point, and from the landing point through the bound) | the same sheared row on the observer's side, under the ray fan (True and Permanent line of sight): the emitter stores the row in the unit (`+0x7A`/`+0x7C`) and, when it is off the sight grid (`main+0x14297`), zeroes the height byte `+0xF8` and stamps nothing — so an aircraft within a few tiles of the north edge reveals nothing to its owner | a unit whose altitude-sheared row leaves the grid: any aircraft at cruise altitude by the north edge | yes. MEASURED: an ARMATLAS at z 40, altitude 175, stored row −2 and lit no cell in rows 0–9 of its owner's LOS map | the point's own row when the sheared one is off the grid, fixed before stock compares it with the stored row and stores it, so the stamp and its later removal use one row by construction; counted | **yes, by design**: what a player sees. Fails closed. |
| the same read in local code: the cursor picker `0x43E490` (`0x43E69D`, `0x43E904`, `0x43EBC6`, `0x43ECDB`, `0x43EE94`, `0x43EFA9`), the build cursor's site test `0x47D2E0` (`0x47D3B8`), the feature helper `0x4658E0` (`0x465942`, `0x46598A`, `0x465A17`, `0x465A63`: both corners), the radar rebuild `0x466DC0`'s projectile dots (`0x46725F`, `0x467294`, `0x467340`, `0x467375`), the particle leaves `0x473590`, `0x473A00`, `0x474170`, `0x4745E0`, `0x475470` (two each) and positional sound `0x47F300` (`0x47F431`, `0x47F476`) | the same sheared row, deciding the pointer's sprite over a point, the build cursor's green, and whether a feature, a particle or a projectile's minimap dot is drawn and a sound played | a feature, particle or sound high up near the north edge. The cursor's point is the world point under the pointer, whose sheared row is the pointer's own row, so on the map it is inside the grid | yes for particles and the radar's dots (MEASURED: fire and smoke near the north edge, 679 342 and 2 395 955 own rows taken in one fight, the dots 2) | the point's own row when the sheared one is off the grid; `0x474B80`'s two copies stay stock, since nothing calls it | no: a cursor, a draw, a sound. Local: the 27 are skipped together. |
| `0x49BEE8` in the projectile draw pass `0x49BE60` (the local player's view of a projectile, the same read inlined beside its call of `0x408090`) | the same altitude-sheared row, so a projectile high up near the north edge is neither drawn nor posed by the engine | any projectile at altitude within a few tiles of the north edge, under True line of sight | yes, in the engine's frame (the golden source; `renderer=gdi` shows it). MEASURED: 24 own rows taken in the same fight | the point's own row when the sheared one is off the grid | no: a draw. Local: skipped alone. |
| `0x490C5A` (the wind updater `0x490C40`'s draws), `0x491903` (the level load's call of it), `0x4982CA` (the loader thread's start) and `0x4C98FD` (the engine's one `SetSessionDesc`) | each peer draws the wind's schedule from its game thread's CRT `rand` (seeded by WinMain's `srand(time(0))` at `0x49E8BB`; `rand` keeps its state per thread) and its speed and heading from the sim RNG (seeded from `QueryPerformanceCounter` at `0x497180`), so the peers of a network game each have a wind of their own | any network game: the schedule diverges on every map, so the changed flag and the wind generators' `SetSpeed`/`SetDirection` fire on different ticks; the heading diverges whenever the speed is not 0, even at a range below 2 (`0x490CC8` tests the speed); the speed wherever the range is 2 or more | yes. MEASURED: two peers paused at GameTime 825 read speed 2525, heading `0xF5C6` and 1498, `0xC827` | the three draws from our own generator by stock's rules, seeded at every level load; in a network game (the engine's own test, `GameingState +0` = 3) from DirectPlay's session instance GUID (read with `GetSessionDesc` on the game thread before the loader thread starts, validated; the host's ID can change during the load, the GUID cannot; the engine's one `SetSessionDesc`, `0x4C9903`, is made with a copy of its descriptor carrying the GUID DirectPlay holds, so no call the engine makes can move it) and a hash of the map's name, else from the engine's copy of the GUID, else the hash alone; otherwise from the performance counter | **yes, by design**: projectiles drift by the wind every tick, and so does the fire spread; every wind generator's income and its script calls follow it. Fails closed. |
| `0x42CF5E` in the unit-def parser `0x42BF40` (the `YardMap` fill) | a missing `YardMap`, or one that ends on an invalid char, fills the remaining cells from past the string's NUL, off each peer's own stack | a mod structure (BMcode 0) with such a string | no retail unit: all 126 retail yardmaps end on a valid char. MEASURED on scratch copies: `"oo?"` took `2f2f31313100002b2b…` from the stack | the parse stays inside the string: a cell past it takes the last valid char's byte, or `o` when there is none; 2440 retail cells byte-identical | **yes, by design**: placement, occupancy and pathing. Fails closed. |
| `0x43A58D` in the saved-game order loader `0x43A420` | an order saved without its name falls back to its index and walks the order table one record past its end (`jbe`), and an index not found keeps the count, 68 or 69, as the order's type | a foreign or damaged save | no: this exe's writer always stores the name | `jbe` → `jb`, and not found goes to "Ready" (`0x43A552`) | no: a malformed input's fate. Local: skipped alone. Not run. |
| `0x439D41` in the stockpile bar `0x439D20` | divides the build's progress by the slot weapon's reload `+0xE4` with the slot index unbounded and the divisor untested | a stockpile weapon with no `reloadtime`, an unarmed slot, or a remote unit whose type differs on this peer | no: every stock stockpile weapon's `reloadtime` is 120 to 180 | an index above 2, a NULL weapon or a zero reload draws no bar | no: a HUD draw. Local: skipped alone. Not run. |
| `0x438EDE` in `DrawRangeCircle 0x438EA0` | a radius of 1 gives N = 0 segments, and `0x438EEE` divides `0x10000` by it; the guard catches only a negative N | a mod unit with a range of 1 | no stock unit | N = 0 draws no circle and no label | no: a HUD draw. Local: skipped alone. Not run. |
| `0x49697B` (the frame `0x496790`'s `call 0x48BAE0`) and `0x499226` (the in-play handler `0x499200`, after the mouse's world position) | an order (`main+0x2CC3`: a build placement `0x0E`, or a command mode such as MOVE 2 or ATTACK 3) stays armed when no unit is left that its click would order: nothing on the selection's death (`0x491D70` writes neither byte), a group recalled by a key, or a deferred drop writes the byte, so every left press goes to the order and orders nobody — a placement keeps its square and the build ghost on the pointer, a command mode swallows the click | the units dying, captured or given away, or deselected by a group recall, with an order armed | yes: ordinary play. MEASURED 2026-09-26 (`tools/b6-tracked-death.sh`, `tools/b9-command-mode.sh`) | for a placement the click's walk (`0x419755..0x41976A`), for a command mode its first test (a selected unit, `0x48CFDF`), both bounded, run at both points, and with nobody to order the right button's cancel `0x499100` disarms, silently — never while `main+0x531` is NULL (`0x499100` reads the menu unguarded at `0x49913B`), and deferred while the engine's own modal test (`0x37EBE & 0x865` or `0x2BEE & 0xE0`, `0x491D76..0x491D8D`) holds; a selection whose units cannot carry a command mode keeps stock's behaviour | no: the byte is read only by the UI. Local: skipped alone. B9 |
| `0x489C71` in the hit sender `0x489BB0` (the store of the amount into the hit record's word) | the amount is an int and the record keeps a word: the damage path subtracts it signed, the paralyser and the heal read it unsigned, so a hit past 32 767 wraps into a gain | a hit above 32 767 after the armour and veterancy scaling: the D-gun of a commander at level 2 or more (30 000 raised by its own level), or a modded weapon | yes. MEASURED (`scenarios/b7-dgun.json`): a commander with 25 kills left an ARMMSTOR at −11 135 = 1 329 + 2 · 26 536 − 65 536, two hits that each raised it | the store saturates into its reader's range, −32 768..32 767, or 0..65 535 for the kinds 2 and `0xA`; the identity inside it | **yes, by design**. Fails closed. |
| `0x4386B9` and `0x4386D8` in the unit reclaim's step `0x438650` (the product and the quotient) | the workertime, `(kills + 5)/5`, the target's MaxHitPoints and 15 are multiplied in 32 bits and read unsigned, so the product wraps past 2³² − 1 (the quotient cannot reach 2³¹ in stock; once the product is exact it could, and `_ftol` returns only its low dword, so the fix clamps it) | a veteran reclaiming a unit with many hit points: ARMCOM (workertime 300) on a CORKROG from 155 kills, factor 32 | yes. MEASURED (`scenarios/b10-reclaim-wrap.json`, `tools/b10-reclaim-wrap.py`): the step at 155 kills is 1 where the formula gives 486, and 145 at 1000 kills where it gives 3 058 | the product computed as an unsigned 64-bit value, saturated at `INT64_MAX`, and the quotient clamped to 2³¹ − 1 before `_ftol`; the engine's own x87 operations unchanged, so every product below 2³² gives stock's step bit for bit (15 and 471 at 0 and 150 kills, measured on both builds) | **yes, by design** (B10). Fails closed. |
| `0x489BF3` in the hit sender `0x489BB0` (the veterancy reduction) | a call of 30 000 or more kills outright (self-destruct, a defeat, a dying transport's cargo, the D-gun) and skips the armour reduction, but the veterancy one still runs, so a veteran above 24 000 HP survives it | a veteran CORKROG (29 918 HP, the only retail unit above 24 000), or a keyed type at a high level | yes. MEASURED (`scenarios/b7-word-outright.json`): a CORKROG with 25 kills survived its own self-destruct at 5 918 HP | the veterancy reduction is skipped by the armour one's test of the caller's amount (`[esp+0x24]` ≥ 30 000) | **yes, by design**: a veteran dies to what kills a recruit, a D-gun included. Fails closed. |
| `0x4673B1` in the radar rebuild's projectile markers `0x467300..0x467406` | the marker for a `targetable` or `interceptor` projectile out of the local player's sight tests its attacker's owner through `proj+0x52`, and a meteor has no attacker | a meteor weapon with `targetable=1` or `interceptor=1` out of sight (True line of sight, unmapped) | no: no retail weapon is one. MEASURED (`scenarios/b7-radar-hail.json`): an access violation at `0x4673B4` reading `0xFF`, `ECX = 0` | no attacker is not the local player's | no: a marker. Local: skipped alone. |
| `0x49A01B` in the projectile's damage step (its gate `0x49A01B..0x49A047`), and the calls at `0x49DF7D` (the shower `0x437DE0`'s spawn) and `0x49D307` (the `0x0D` receiver's meteor branch) | every peer runs its own shower, broadcasts each stone, and computes every stone's damage, since no peer marks the stones' owner record 10 remote, so a stone's hit is applied once by its owner and once more for each other peer | any network game on a map with a meteor shower | yes. MEASURED on two peers (`scenarios/b7-mp-host.json`, `b7-mp-join.json`): all 19 hits were computed on both, and each storage lost exactly twice what the stones dealt | the spawn marks its stone's `+0x62` 0 and the receiver 1 (the field is the firing piece the shooter's `Query*` script answers, which a stone does not have), and the gate sends a received stone where stock sends a remote projectile; a stone is computed on the peer that spawned it, as a shot is on the firer's | **yes, by design**. Every peer still rains its own shower. Fails closed. |
| `0x434D87` in the map list builder `0x434BF0`, and `0x430B87` in the options loader (the saved `SkirmishMap`) | a map is listed by its `.ota` alone, so a map whose `.tnt` the install lacks is offered in the skirmish and battle-room lists and can be the saved skirmish map; opening it stops the game on a box naming `Maps\<name>.TNT` | a mod that ships a map's `.ota` without its terrain: Total Mayhem 11.3.0 on the Steam install, nine maps, the first of them the default on a fresh key | yes: on the retail install with such a mod. MEASURED 2026-09-27 on Windows (the box) | the builder also asks whether `Maps\<name>.TNT` opens through the archive layer (`0x4290F0`, `0x4BB5B0`, `0x4BB5D0`) and takes its own refusal branch `0x434EA9` when it does not; a saved value naming such a map is replaced as a missing one is. MEASURED under Wine: both lists 109 → 100, exactly the nine, the saved map moved to the first with terrain; on Windows the list 109 → 100 and Start into the game where the previous build stopped on the box | no: which maps are offered; whether a network peer has the host's map is decided from the terrain file itself (`0x448EAE`), not from this list. Local: skipped alone. |

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
