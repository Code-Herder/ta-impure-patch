# TA engine-modifying projects — community-archive sweep

Scope: projects for **original Total Annihilation (Cavedog, 1997)** that add engine features by
patching / hooking / wrapping the closed-source binary. Engine re-implementations (Spring, Recoil,
Zero-K, BAR, TA3D, OpenRA, Tech Annihilation) are **excluded** throughout.

Sweep date: 2026-08-31. Sources: tauniverse.com + subdomains, files.tauniverse.com, PCGamingWiki,
GitHub API, ModDB, Nexus, Steam discussions. Includes a **first-hand teardown of a shipped patch**.

---

## Headline

There is **one dominant engine-patch stack**, not many independent hacks. It is the
**TA Community Patch / TA Unofficial Patch 3.9.x**, whose active engine core is **TADR (`tdraw.dll`)**.
It is a DLL that **runtime-patches `TotalA.exe`'s machine code** (VirtualProtect + 5-byte E9 inline
detours against hard-coded addresses). Every modern mod — Escalation, Zero, Mayhem, ProTA, Twilight —
ships a **build-configured variant of that same DLL** and sits on top of the patch.

**TADR's last commit is dated 2026-08-31 — today.** This is a live project, not a 90s relic.

---

## Candidate table

| # | Name | Author / team | Dates | Engine feature(s) added | Mechanism | Link | Conf |
|---|------|---------------|-------|--------------------------|-----------|------|------|
| 1 | **TADR / `tdraw.dll`** — the engine core | *tanvanman*; lineage SY_Yeha → Xpoy → Rime; contribs Axle1975, FunkyFr3sh, tagROCK, TAG_Venom | fork of svn.riouxsvn.com/tadr; **active, last commit 2026-08-31** | Projectile/explosion pools 300→3000; model-piece slots 100→1000; unit limits; **weapon IDs >255**; **new weapon TDF flags** (`nottoair`, `surfacefire`, `notoverwater`); megamap; building rotation; nanoframe build preview; mex/wreck snapping; drag-queued orders; **per-unit veterancy via FBI keys**; neutral/per-player map unit spawns; lag-switch mitigation; unicode chat; vote-to-reject; `+autoteam`; CRC anticheat | **DLL that rewrites the exe at runtime**: `VirtualProtect` + 5-byte `E9` inline detours + trampolines on hard-coded addresses (e.g. `0x4c9840`); entry splice at `0x004E6FA0` to escape DllMain loader lock | [github.com/tanvanman/TADR](https://github.com/tanvanman/TADR) | **[VERIFIED]** |
| 2 | **TA Unofficial Patch / TA Community Patch v3.9.02** | TAUniverse community — *Skirmisher* (installers), *xpoy* (megamap), *Admiral_94* (exe work), *N72*, *Axle*, *gamma/gammata*, *Thaldren* | 3.9.01 ~2012 → 3.9.02b ~2013 → redistributed 2022-2026 | Unit limit **250→1500** (max 6553); unit-ID limit **512→16000**; **weapon-ID limit 256→16000**; pathfinding cycles **1333→66650**; SFX limit **400→20480**; unit model buffer **600²→1280²**; MegaMap w/ wheel zoom; replay record/playback; 3D positional sound; sound channels 8→128; skirmish players 4→10; arbitrary resolution; double-click select; expanded share menu | **Patched `TotalA.exe`** (string `v3.9.02`) **+ replaced native DLLs `tplayx.dll` / `tdraw.dll` / `tmusi.dll`**. `tplayx.dll` exports `zInitCode_CoreExePatching`, `UseWeaponIdPatch`. Ships `modstool.exe`, `move_maps.dll`, `audiere.dll`, cnc-ddraw | [t=43735](https://www.tauniverse.com/forum/showthread.php?t=43735) (login-walled) · [patch.tauniverse.com](https://patch.tauniverse.com/ta-patch/) · [PCGamingWiki](https://www.pcgamingwiki.com/wiki/Total_Annihilation) | **[VERIFIED]** |
| 3 | **`tplayx.dll` / TA Demo Recorder (TADR lineage)** | Swedish Yankspankers (*Fnordia*, *SJ*, *Yeha*), later *Xon* | 99b2 (2008) → 1.0RC2 (2009) → maintained in TADR to 2026 | Replay record/playback + `SERVER.EXE` replayer, in-game whiteboard markers, multiple AIs per player, LOS sharing, lobby/netmsg extensions | **DirectPlay proxy DLL** (Delphi, `Dplayx.dpr`) exporting `DirectPlayCreate`; also injects code (`InitCode_CoreExePatching.pas`) | files.tauniverse.com → demo-recorder | **[VERIFIED]** |
| 4 | **Total Annihilation Patch Loader** | *FunkyFr3sh* | last commit 2024-05-02, in active distribution | Applies the byte patches **without** a hex-edited exe: custom registry path, custom ini/gp3 names, pathfinding raise, `+atm` fill-resources, AI commander order-reset fix, DirectX popup removal, battleroom version byte, F10 debug mode | **`dplayx.dll` proxy**: validates a 10-byte signature at `base+0x10000`, patches memory, `LoadLibrary("tdraw.dll")`, redirects `DirectDrawCreate` at `0x0047BFA2`/`0x004B55FB` | [github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader](https://github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader) | **[VERIFIED]** |
| 5 | **cnc-ddraw** | *FunkyFr3sh* | active 2026-08-31 | Borderless/windowed, upscaling, OpenGL shaders (CRT/xBR/lanczos), Alt+Enter, OBS capture, mouse sensitivity | Drop-in `ddraw.dll` reimplementation, sits **behind** `tdraw.dll` | [github.com/FunkyFr3sh/cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) | **[VERIFIED]** |
| 6 | **TA Zero** | TA Zero team (*Vohvelieläin* et al) | patches → 2020; **Alpha 5, 2024-12-24** | **Functional flashing/recharging shields**, pathfinding cycles ×50, unit limit 500→1500, unlimited simultaneous sounds, raised particle limits, new AA/bombing/sniper weapon behaviours, full-screen megamap | Built on the TA Patch; **own registered `TotalA.exe`** + own `zdraw.dll` variant | [zero.tauniverse.com/features](https://zero.tauniverse.com/features/) | **[VERIFIED]** |
| 7 | **TA: Escalation (TA:ESC)** | *WotanESC*, TRO (merger of Talon + TAWP); Axle, xpoy, Admiral_94, N72 | Beta 3.3 (2009) → ESC X (2026-02) → **GOLD 10.2.0, 2026-08-08** | **Deflective shield generators**, mass teleport gates, megamap, improved pathfinding for large T3 units, veterancy, share-abuse guard, repair-rate exploit fix, aircraft wrecks fall | **Own patched `totala.exe`** + **own DLL build: `taesc.dll` and `edraw.dll`, both 680,448 bytes — byte-identical in size to OTA's `tdraw.dll`**, i.e. per-mod TADR builds. Also `escalation.ini` | [taesc.tauniverse.com](https://taesc.tauniverse.com/) · [ModDB](https://www.moddb.com/mods/total-annihilation-escalation) | **[VERIFIED]** |
| 8 | **Total Mayhem** | *Mayhem Inc.* | since 2001; 6.92 (2013) → **11.3.0, Jun 2024** | **12-button builder interface** (vs stock 6) with hotkey overlays, 32-tile off-map AA, crash/lockup fixes, unit AI fixes | Installer since 6.92 **auto-downloads TA Unofficial Patch 3.9.02**; own `mdraw.dll` variant | [ModDB](https://www.moddb.com/mods/total-mayhem) · [mayhem.tauniverse.com](https://mayhem.tauniverse.com/) | **[VERIFIED]** |
| 9 | **gpgnet4ta** (TA Forever launcher) | *Axle1975* / ta-forever | created 2020-11, active 2026-08 | Tunnels all DirectPlay traffic via one UDP port for ICE/hole-punching; late join; **live replay streaming**; demo compiler; game-state monitoring | **Does NOT patch the exe.** Userspace DirectPlay proxy + `CreateFileMapping` shared memory to read `tdraw.dll`'s elimination/start-position data. Ships `online.dll` | [github.com/ta-forever/gpgnet4ta](https://github.com/ta-forever/gpgnet4ta) | **[VERIFIED]** |
| 10 | **Cavedog extension-DLL API** | Cavedog (1997) | 1997 | TA has a **built-in official plugin ABI**: `online.dll` scans for `ta*.dll` and calls `TotalAExtVersion` / `TotalAExtAction` / `TotalAExtGetButtonText` | Official, **no patching required** | strings in `gpgnet4ta/online.dll` | **[VERIFIED]** |
| 11 | **Click Queue Extension (`jtaext`)** | *Jonathan Rennison* | 2006 | Extends/configures build-queue click behaviour | `jtaextcfg.exe` patches `TotalA.exe` at `0x19FBB` to load `jtaext.dll` — **rides Cavedog's own `ta*.dll` API** | files.tauniverse.com → click-queue-extension-patch | **[VERIFIED]** |
| 12 | **5000 / 1500 / 500-unit patchers** | *SY_Yxan* (Swedish Yankspankers) | 1999–2010 | Raises max unit count (`0x01F4`→`0x1388`) | `patch.bat` + `MALEX.EXE` hex search-and-replace on `TotalA.exe`, backs up to `tabackup.exe` | files.tauniverse.com → 5000-unit-patcher | **[VERIFIED]** |
| 13 | **RomHaxxor + TA Pathfinding Fix** | TAUniverse (t=42529) | 2008 / 2012 | Pathfinding cycles `0x535` (1333) → `0x1045A` (66650) | Generic exe byte-patcher + `.cfg` offset file | files.tauniverse.com → ta-pathfinding-fix | **[VERIFIED]** |
| 14 | **`+atm` 10000 / TA Speed / `+shootall` / skirmish-resource patchers** | *bLoBbY* (zzymyn) | 2004–2006 | `+atm` gives 10000 not 1000; removes the +10 game-speed cap; `+shootall` on by default; removes skirmish metal/energy cap | Small C-source exe patchers with published byte offsets (speed: `0x901FD` `7E`→`EB`) | files.tauniverse.com → unofficial-patches | **[VERIFIED]** |
| 15 | **Xpoy's weapon-ID hack / TA Interface Upgrade** | *Xpoy* | 2002 | **Weapon-ID limit crack**, hotkeys, unicode font, early megamap | `ddraw.dll` proxy (exports `DirectDrawCreate`, `DirectDrawCreateEx`) — **the ancestor of TADR** | files.tauniverse.com → old-weapon-id-hack | **[VERIFIED]** |
| 16 | **Xon's DLL files** | *Xon* | ddraw 2004, dplayx 2006 | Direct predecessor of the tdraw/tplayx stack (ships a `.map` symbol file) | `Dplayx.dll` + `ddraw.dll` proxies | files.tauniverse.com → xon-dll-files | **[VERIFIED]** |
| 17 | **The Secret Forces** | *Shelby123456789* | EA 2014 → v0.7.7a (Jul 2017) | Nanotower with extended build range; addon race | ModDB states it "includes **its own custom patched exe**"; requires TA Community Patch | [ModDB](https://www.moddb.com/mods/the-secret-forces) | **[VERIFIED]** |
| 18 | **Thaldren Modular-Patch** | *thaldren* | 2025-10 → 2025-12 | Fixes DirectX init failures, full-screen sizing; DPLAYX/WINMM modules WIP | Standalone `ddraw.dll` drawing via Win32 API | [github.com/Thaldren-Updates/Modular-Patch](https://github.com/Thaldren-Updates/Modular-Patch) | **[VERIFIED]** |
| 19 | **totala-re** | *ioma8* | last commit 2025-10-17 | Not a patch — the **only public TA reverse-engineering documentation set**: entry `0x4e6fa0`, main loop `0x49e830`, HPI/SQSH/TMH decoded | Static analysis + Python tooling | [github.com/ioma8/totala-re](https://github.com/ioma8/totala-re) | **[VERIFIED]** |
| 20 | **TAMusic98** | *winlith* | 2024 | Period-correct `win32.dll` so GOG/Steam TA runs on Win98. Incidentally proves **GOG/Steam `TotalA.exe` is itself publisher-patched** to import `win32.dll` instead of `winmm.dll` | Replacement wrapper DLL | [github.com/winlith/TAMusic98](https://github.com/winlith/TAMusic98) | **[VERIFIED]** |
| 21 | **Multicore patch** | unknown | 2008 | Pre-patched exe for multi-core CPUs | Distributed as a modified `TotalA(Multicore).exe`, no source | files.tauniverse.com → multicore-patch | **[VERIFIED]** (existence; *which* bytes unverified) |
| 22 | **DxWnd** | gho / DxWnd team | ongoing | Generic windowing/compat API hooking; ships an official TA profile | Runtime API hooking of a launched process | [DxWnd.reloaded](https://github.com/DxWnd/DxWnd.reloaded) → `Total Annihilation.dxw` | **[VERIFIED]** (generic) |
| 23 | **IPXWrapper** | *solemnwarning* | active 2026-08 | Restores LAN/IPX discovery on modern Windows | DLL wrapper in game dir | [github.com/solemnwarning/ipxwrapper](https://github.com/solemnwarning/ipxwrapper) | **[VERIFIED]** (generic) |
| 24 | **Axle1975's TAF ddraw patch** | *Axle1975* | ~2022+ | Synced windspeed between internet players, autoteam/randomteam, auto player positioning, min resolution 1024x768 | Reported ddraw-level patch — likely the same TADR lineage | Steam TA discussions / TAF distribution | **[LEAD]** |
| 25 | **"Hacking TotalA.exe"** (t=45399), **"Hacking TA..."** (t=41608, 9+ pages), **"Totala.exe & 5000 unit fix"** (t=6171) | tauniverse members | various | Presumed disassembly / exe-patching discussion — the community's know-how base | unknown | login-walled forum threads | **[LEAD]** |
| 26 | **ProTA** | *TAG_Venom* + team | first release Jun 2021, v4.4–4.8 | Site claims only a "veteran-led rebalance" — QoL, build hotkeys, minimap colours. **No engine features confirmed**, but ships a patched exe + its own tdraw variant | Patched `TotalA.exe` + DLLs (verified in ProTA4.8.zip) | [prota.tauniverse.com](https://prota.tauniverse.com/) | **[LEAD]** |
| 27 | **TA: Twilight** | *Twilight* (successor to Caydr's Absolute Annihilation) | 2007 → v1.7 (2009); still listed active 2024 | No engine claims found; ships `TWILIGHT.Dropin.Install.zip` | Patched exe folder | [ModDB](https://www.moddb.com/mods/total-annihilation-twilight) | **[LEAD]** |
| 28 | **TA Mutation (TAM), TA-Thing, TA 4.0, TA Config** | various | 2005–2008 | Unknown. TAM's only trace: Skirmisher's `Patch/notes.txt` flags uncertainty about **uninstalling TA Mutation**, in a list of legacy in-game-dir patches → it wrote files into the TA directory | unknown | files.tauniverse.com → unofficial-patches | **[LEAD — weak]** |

---

## PRIMARY-SOURCE TEARDOWN (my own, first-hand)

I downloaded `OTA.Dropin.Install.zip` and `ESCALATION.Dropin.Install.zip` from the gammata repo and
inspected them directly.

**OTA drop-in (49 files):** `TotalA.exe` (1,178,624 b, 2013-09-29, contains the literal string
`v3.9.02`) · `tplayx.dll` (298,496 b) · `tdraw.dll` (680,448 b) · `tmusi.dll` + `audiere.dll` ·
`ddraw_custom.dll` + `ddraw.ini` + `cnc-ddraw config.exe` + `Shaders/*.glsl` · `TADemo/SERVER.EXE` ·
`Settings.ini`, `ChatMacro.ini`, `Icon/iconcfg.ini`, `rev31.gp3`.

Strings in `tplayx.dll`: `zInitCode_CoreExePatching`, `UseWeaponIdPatch`, `Using weapon ID patch.`,
`uses TA Unofficial Patch`, `Recorder version:`, and
`TA Hook is now obsolete - the functionality is built right into TA Demo itself`.

**Escalation drop-in (28 files):** **no `TotalA.exe`**, but `edraw.dll` **and** `taesc.dll`, *both
exactly 680,448 bytes* — the same size as OTA's `tdraw.dll`. This is direct confirmation that each
mod ships a rebranded build of the same TADR engine DLL.

**Verified feature table from the shipped, self-documenting `Settings.ini` (stock 3.1 → 3.9.02):**
UnitLimit 250→1500 (range 20–6553) · UnitType (unit IDs) 512→16000 · **WeaponType (weapon IDs)
256→16000** (multiplayer use gated behind `MultiGameWeapon=FALSE`, "not yet compatible with
Replayer") · AISearchMapEntries (pathfinding) 1333→66650 · SfxLimit 400→20480 · CompositeBuf
600²→1280² · FullScreenMinimap (MegaMap) + wheel zoom, per-sensor range rings, under-attack flashing,
custom player dot colours/PCX markers · Sound Mode mono→3D positional · MixingBuffers 8→128 ·
NumSkirmishPlayers 4→10 · DisplayModeWidth/Height override (e.g. 1920x1080) · DoubleClick select ·
ShareDialogExpand · whiteboard/build-ring hotkeys · on-screen income display.

**Note: nothing in the base patch adds shields.** Shields come from the mods (TA Zero's
"flashing/recharging shields", ESC's "deflective shield generators").

---

## Two delivery mechanisms coexist

1. **Legacy:** hex-edit the PE **import table** of `TotalA.exe` so `DDRAW`→`TDRAW`,
   `DPLAYX`→`TPLAYX`, `WINMM`→`TMUSI`. The patch DLLs then load at process start and detour code.
   (Confirmed by dumping the import table of a distributed community-patch exe.)
2. **Modern:** keep a **stock 3.1 exe** and drop in FunkyFr3sh's `dplayx.dll` proxy, which applies the
   byte patches at runtime and chain-loads `tdraw.dll`. TAF's current OTA package
   (`Total Annihilation Modern OTA Patch.zip`, 2026-02-25) contains **no `TotalA.exe` at all**.

ProTA and Mayhem still bundle a patched exe; TAF has moved to the runtime route.

---

## Refutations — NOT engine mods

| Name | Verdict |
|------|---------|
| **Talon / Talon Race** | **Content-only.** ModDB states it is an HPI/UFO addon race and "does not modify the engine or executable". Its shields/teleporters/motherships are **COB-script tricks**. Absorbed into TA:ESC. |
| **TA Hook 0.85** | **Not a patch.** Its own readme: it is "not a patch to TA, but rather just a 'mouseclick simulator'" — an external tray app. The *name* was later reused for a reimplemented feature inside `tdraw.dll`. |
| **TAUCP** | **Name collision — beware.** `TAUCP` on tauniverse means **Total Annihilation *Units Compilation Pack*** by *CyberKewl* (Malcolm Lim), a discontinued unit pack (v2.1) — **not** a community patch. Some sources loosely use "TAUCP" for the TA Community Patch. Disambiguate before citing. |
| **Absolute Annihilation** | Content-only (Caydr; unit/balance). Absent from ModDB and Nexus. TA:Twilight is its successor. |
| **Uberhack** | Real (named in the TA:ESC FAQ as an influence) but **zero hits on ModDB/Nexus**; no engine evidence. Treat as content-only pending a File Universe check. |
| **TA:Enhanced** (v0.8v, 2020), **TA Frenzy**, **TA: Devolution**, **Total Battletech**, **CZTA**, **Gundam Annihilation**, **TA Kingdoms Plus** | Content-only. Several *require* the community patch but add no engine code. |
| **TA Sound Fix** | A `.reg` file setting `MixingBuffers=0x40`. Registry config only. |
| **Extended Line of Sight** | Ships one file, `gamedata/LOS.TDF`. Data-only. |
| **Expanded Battleroom / Map Selection GUI** | Ships only `.GUI` and `.pcx`. Data-only (TA's GUI is data-driven). |
| **TAF client** (`downlords-taf-client`, Java) | Does not patch or hook the exe; it launches TA and **distributes** the patch DLLs as featured-mod packages. |
| **`pinnbox/Totaler_Annihilation`** | From-scratch RTS. Re-implementation, excluded. |
| **`cjg38340/TotalA_exe-Rewrite`** | One-line README, no code. Effectively nonexistent. |
| **`MaxUint/TAAnywhere`** | Electron mod installer. No engine work. |
| **Tech Annihilation** | Spring-engine project. Out of scope. |

## Zwzsg — status: **unresolved**

The only primary evidence reached is the **TA:ESC FAQ**, which credits Zwzsg among "intrepid
developers" for **advanced/experimental unit scripting (especially transports)** — i.e. COB scripting,
*not* executable hacking. No released Zwzsg exe-patching tool was located; `zwzsg.free.fr` does not
resolve. His reputed exe work, if any, is most likely inside the login-walled "Hacking TA..." threads.
**Do not assert he is an exe hacker on current evidence.**

## Access limitations

- **tauniverse.com forums went members-only on 2025-11-18** (front-page post by *gamma*: bot load),
  behind Cloudflare. Threads 45399 / 41608 / 6171 and subforum f=170 are unreadable without an account.
  No Wayback captures were retrievable.
- **reddit.com is blocked** for fetching in this environment; the r/TotalAnnihilation angle is unswept.
- The session's 200-call WebSearch budget was exhausted mid-sweep.
- **files.tauniverse.com is NOT Cloudflare-gated** and yielded the complete historic unofficial-patch
  archive — use it as the primary entry point next time.

## Highest-value follow-ups

1. **`github.com/tanvanman/TADR`** — the live engine patch. `src/DDraw/tdraw.txt` is a dated changelog
   of every engine feature added; `src/DDraw/tamem.h` (~57 KB) + `ta info.txt` are the community's
   working **memory map of `TotalA.exe`**. This is the single highest-value artefact found.
2. `github.com/ioma8/totala-re` — independent RE documentation to cross-check that memory map.
3. `Mods/notes.txt` in Skirmisher's repo — documents the `modstool.exe add/remove/update` syntax
   including per-mod **SFX limit** and **unit limit** parameters: the concrete API by which the shared
   engine patch exposes new limits to mods.
4. Diff a patched `Total Annihilation.exe` against stock `TotalA.exe` — the whole patch surface.
5. Get forum access for threads 45399 / 41608 / 6171 to resolve Zwzsg and the historic hacks.
