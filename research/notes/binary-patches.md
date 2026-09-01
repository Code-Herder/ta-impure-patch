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
