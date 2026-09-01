# TA Unofficial / Community Patch 3.9.02

Scope: the original Cavedog 1997 `TotalA.exe` only. Evidence tags: **[VERIFIED]** = observed
directly in a file I downloaded and inspected, or read verbatim from a primary document.
**[CLAIMED]** = asserted by the authors' own documentation but not independently confirmed.

## Summary

The TA Unofficial Patch (a.k.a. TA Community Patch) is a comprehensive, installer-delivered
update to retail Total Annihilation, published by TA Universe. v3.9.02 Beta is dated
**October 2, 2013** [VERIFIED — patch readme]. It is explicitly *not* a mod: "This is a patch,
NOT a mod, and does not change the game balancing in any way; it only adds new features and
fixes technical issues" [VERIFIED — readme]. The 3.9.xx series was framed as beta testing
leading to a v4.0 non-beta release that, as far as the distributed files show, never shipped.

Mechanically it is **not** a Cavedog-plugin-based extension. It works by (a) hex-editing the
retail `TotalA.exe` import table so the game loads community DLLs instead of system ones, and
(b) doing all the interesting work inside those replacement DLLs at runtime. I confirmed this
directly from the binaries (below), including the exact import-string VA the community reports.

The patch supersedes and absorbs a long list of older standalone community hacks: Cavedog's
own v3.1 patch, TA Demo 0.99b2 / 1.0.0.545, xpoy's TA Interface Upgrade (`ddraw.dll`), the
NoCD and NoCD-Music patches, "Expanded Battleroom and Map Selection GUI", the 500/1500/5000
unit-limit patches, the TA Sound Fix (mixingbuffers), the TA Pathfinding Fix, the LOS tables
fix, the `+atm 10000` patcher, and the Multicore Patch [VERIFIED — readme + `Patch/notes.txt`].
Detected copies are moved to a backup folder and restored on uninstall [CLAIMED].

## Version history & changelog

Only two releases exist in the 3.9.x line [VERIFIED — directory listing + readme's own
"Version History" section]:

| Version | Date | Installer | Size |
|---|---|---|---|
| v3.9.01 Beta | 15 Apr 2012 | `TA_Patch_3901.exe` (monolithic) | 162 MB |
| v3.9.02 Beta | 03 Oct 2013 | `TA_Patch_3902.exe` + `TA_Patch_Resources.exe` | 22 MB + 153 MB |

**v3.9.02 split the distribution in two** [VERIFIED — readme]: "TA Patch Resources" (~150 MB,
rarely updated: MP3 soundtrack, the 16 downloadable Cavedog maps, TA Features 2013) and "TA
Patch" (~20 MB, updated often, requires Resources). Mods then in development at TAU — TA:
Escalation, TA Zero, Total Mayhem — were to be built on the patch as a base.

### v3.9.02 — engine changes
- `TotalA.exe`, `tdraw.dll`, `tplayx.dll`, TA Demo all bumped to 3.9.2.0.
- Unit/weapon ID default limits raised to 16000 to match Conflict Crusher's extended-ID setting.
- Fixed antialiasing (red outlines, outline expansion, incorrect gamma) and shading (loss of
  transparency, incorrect gamma) — both now gamma-correct.
- Fixed AI Commanders resetting build orders on every projectile impact, and target-thrashing
  when attacked by many enemies.
- Fixed LOS-related issues *between* `tdraw.dll` and `tplayx.dll`.
- Fixed bugs in the multiplayer weapon-ID crack.
- Fixed fullscreen graphics corruption above 1280x800 on Nvidia Kepler (600-series) and newer,
  conditional on VSync staying enabled.
- Many low-framerate cases fixed.

### v3.9.02 — GUI / content / interface
- **The megamap** (see below) plus its icon set, `iconcfg.ini` customisation and CTRL+F2 menu.
- Expanded multiplayer resource-sharing menu.
- `+unitname` unit spawning and INSERT command-repeat (cheats-enabled games only).
- CTRL+SHIFT+B/F/S restored the original CTRL+B/F/S semantics after those were remapped.
- Customisable player colours for whiteboard/minimap/megamap.
- Content moved out to the Resources package; `TADemo.ufo` retired into `rev31.gp3`.
- Installer: removed the TA-install check and all VBScript.

### v3.9.01 — the foundation
This is where nearly all the limit raising landed: the pathfinding, special-effects, unit-ID,
weapon-ID, unit-model-size and unit-limit "adjusters"; the weapon-ID packet-length fix
(shipped disabled); Alt+TAB crash fix; developer mode synced to cheat mode; `totala.ini`
replaced by `TA.ini`; debug key F11→F10; DirectX check suppressed; `totala_log.txt` removed;
`tmusi.dll` + `audiere.dll` for MP3 music; expanded battleroom and map-selection GUIs added to
`rev31.gp3`; double-click selection and W/B/Y selection filters; PrintScreen screenshots.

## Engine limits raised (with offsets where published)

The authoritative published record is the patch's own INI (`TA.ini` in 3.9.02; the file is
named `Settings.ini` in later repackages). Every figure below is **[VERIFIED]** — read
verbatim out of that shipped INI, which states both the v3.1 stock default and the new default:

| Setting | TA v3.1 | Patch default | Notes |
|---|---|---|---|
| `UnitLimit` | 250 | 1500 | settable 20–6553; "DO NOT set higher than 6553 or TA will crash" |
| `AISearchMapEntries` (pathfinding cycles) | 1333 | 66650 | |
| `SfxLimit` | 400 | 20480 | marked "still experimental" |
| `UnitType` (unit ID limit) | 512 | 16000 | |
| `WeaponType` (weapon ID limit) | 256 | 16000 | |
| `X_CompositeBuf` / `Y_CompositeBuf` | 600x600 | 1280x1280 | unit model draw buffer |
| `MixingBuffers` | 8 | 128 | ≥33 = unlimited simultaneous sounds |
| `NumSkirmishPlayers` | 4 | 10 | |
| `Sound Mode` | 1 (mono) | 2 (3D positional) | speaker layout from Windows |
| `CDMode` | 4 (custom) | 2 (random) | |
| `SwitchAlt` | 0 | 1 | number keys select groups |
| `DisplayModeWidth`/`Height` | — | off by default | arbitrary resolution override |

Two important design notes from the INI and readme [VERIFIED]: the unit limit is applied **in
real time rather than pre-patched**, so "1500 does not mean patched to 5000 and then limited to
1500, it is actually patched to 1500" — this is why low limits stay stable, unlike the old
500/1500/5000 patchers. And arbitrary resolution works for any mode "available in 8-bit colour
depth"; an unsupported mode crashes the game.

**A published offset, independently confirmed and extended.** The community reports the
`DDRAW.dll` import string at VA `0x004FF618`. I confirmed it exactly: that VA maps to file
offset `0x000FE418` in `.rdata`, and the bytes there are `TDRAW.dll\0` [VERIFIED]. The same
trick appears at VA `0x004FF642` where `TMUSI.dll\0` sits [VERIFIED]. Both substitutions are
length-preserving — `DDRAW.dll` and `WINMM.dll` are each 9 characters, exactly like `TDRAW.dll`
and `TMUSI.dll` — so no relocation or table rebuild is needed. That is the whole hijack.

**A new offset I derived.** The changelog's "Changed default pathfinding cycles in exe to
66650" is directly visible in code. At **VA `0x0040EAD3`** (file offset `0x000DFED3`) the
instruction is `C7 46 48 5A 04 01 00` = `mov dword ptr [esi+0x48], 0x1045A`, i.e. 66650
[VERIFIED by disassembly]. The stock instruction would have carried `0x535` (1333), so this is
a 4-byte immediate overwrite at file offset `0x000DFED6`. I found no `16000` or `6553` dword
immediates in `.text`, consistent with those limits being applied at runtime by `tdraw.dll`
rather than baked into the exe.

## Installation & file layout

`TA_Patch_3902.exe` is a **Caphyon Advanced Installer** MSI-in-EXE with an LZMA `disk1.cab`
payload; ProductCode `{0BDA7A1D-1A75-4AB8-B362-28DF706518D5}`, ProductVersion `3.9.02`,
Manufacturer "Total Annihilation Universe" [VERIFIED — MSI tables].

**It replaces `TotalA.exe` in place.** The premise that it ships a separate
`Total Annihilation.exe` as the game is **incorrect**: the MSI's File table does contain a
`Total Annihilation.exe` (short name `TotalA~1.exe`), but the Shortcut table wires it to the
shortcut *"Configure Display Settings"*, while the *"Total Annihilation"* play shortcut targets
`TotalA.exe` [VERIFIED — MSI Shortcut/File tables]. So `Total Annihilation.exe` is the
**launcher/resolution tool** the readme describes ("set the game resolution, enable or disable
windowed mode, save your resolution setting, and launch the game"), not the game.

Other installed components [VERIFIED — MSI File table]: `tdraw.dll`, `tplayx.dll`, `tmusi.dll`,
`audiere.dll`, `rev31.gp3`, `TA.ini`, `iconcfg.ini`, the megamap `Icon\*.PCX` set,
`3DTA.exe`/`3DTAConfig.exe`/`DTA.exe` (3D Replayer), `SERVER.EXE` + `online.dll` + `HPIUtil.dll`
(TA Demo), `updater.exe`, `readmepatch.txt`, `readme31.txt`, and `remove_junk.cmd` /
`restore_junk.cmd` implementing the backup-and-restore of superseded community files.

Registry footprint [VERIFIED]: `HKCU\Software\TA Patch\{Eye, Launcher, 3dta, TA Demo\Options}`,
`HKLM\SOFTWARE\Wow6432Node\TAUniverse\TA Patch\Version`, the stock
`Software\Cavedog Entertainment\Total Annihilation`, and — notably — an auto-configuration write
into `Software\VB and VBA Program Settings\TA Conflict Crusher\Settings` for Conflict Crusher's
extended-ID setting. The `Eye` subkey is `tdraw.dll`'s hive (e.g. `MegamapKey`, VSync).

The auto-updater manifest is **still live** at
`http://patch.tauniverse.com/ta-patch/TA_Patch_Updater.txt` [VERIFIED — fetched]. It advertises
`ProductVersion=3.9.2.0` and no successor, confirming 3.9.02 is the last release. It also
carries a **stale checksum**: it lists `Size=22778949`, `MD5=54ef63f3112d1000d825d3748fc54995`,
but the file actually served from both `patch.tauniverse.com` and `files.tauniverse.com` is
22,810,263 bytes / MD5 `bee66f65235acabd8857da0157254eec` [VERIFIED — both mirrors return
identical `content-length`]. Anyone writing an integrity check against that manifest will fail.

## Binary inspection findings

Downloaded and statically analysed (never executed). Installer:
`TA_Patch_3902.exe`, 22,810,263 bytes,
SHA-256 `6e9154b21f31e9506c86242fd7e1fef31f14532170e4341f1814747353904f68`.

Patched binaries taken from gammata's drop-in package (SHA-256 of the zip:
`7ac571e4d75d27c269e838fa963470cd273fd2d4d29c58ecc379944ec13ee738`):

| File | Bytes | SHA-256 (truncated) | Version resource |
|---|---|---|---|
| `TotalA.exe` | 1,178,624 | `dbf8c705abc4a90c…` | 3.9.2.0 / "v3.9.02 Beta", CompanyName *Cavedog Entertainment*, OriginalFilename `Totala.exe` |
| `tdraw.dll` | 680,448 | `9ce51279fa236ee3…` | **5.0.0.0**, *vThaldren Developments*, "Unofficial Beta Patch DDraw", OriginalFilename `idraw.dll`, © 2022 |
| `tplayx.dll` | 298,496 | `aefe39696a2e890e…` | 3.9.2.416, "TA Unofficial Patch - Demo Recorder Component", internal name `Dplayx.dll` |
| `tmusi.dll` | 31,744 | `bfd96f5385984e80…` | none |

**Caveat on provenance:** the `tdraw.dll` in that package is a **2022 v5.0.0.0 build**, not the
3.9.2.0 `tdraw.dll` the 3.9.02 changelog describes. The `TotalA.exe` and `tplayx.dll` *are*
3.9.02-era. This is why that package's INI is `Settings.ini` while the 3.9.02 readme documents
`TA.ini` — the drop-in is a later re-spin layered over the 3.9.02 exe [VERIFIED].

**Import tables** [VERIFIED]. `TotalA.exe` imports `TDRAW.dll`, `TMUSI.dll`, `TPLAYX.dll`,
KERNEL32, USER32, GDI32, ADVAPI32, `smackw32.DLL`, `DSOUND.dll`, SHELL32. Strings also
reference `DebugHelper.dll`, `reporter.dll`, `psapi.dll` and `IMAGEHLP.DLL` — a crash-reporter
path absent from stock TA. `tdraw.dll` imports **only** IMM32/USER32/KERNEL32/GDI32/ADVAPI32 —
crucially *not* `ddraw.dll`, so it is a full DirectDraw reimplementation, not a thin shim.

**Exports** [VERIFIED]. `tdraw.dll` exports the complete 22-entry DirectDraw ABI
(`DirectDrawCreate`, `DirectDrawCreateEx`, `DirectDrawEnumerateA/W/ExA/ExW`, `DllGetClassObject`,
`AcquireDDThreadLock`, `SetAppCompatData`, …). `tplayx.dll` exports the DirectPlay ABI
(`DirectPlayCreate`, `DirectPlayEnumerate`, `DirectPlayLobbyCreateA/W`, `gdwDPlaySPRefCount`).
`tmusi.dll` is a pure proxy: ~200 forwarder exports to `winmm.*`, plus imports of `audiere.dll`
and MSVCR90/MSVCP90 — that is the MP3 music redirection.

**No Cavedog plugin ABI.** I searched every binary for `TotalAExtVersion` and `TotalAExtAction`
and found **zero** occurrences [VERIFIED]. The patch does not use Cavedog's documented plugin
interface at all; it is entirely import-table hijack plus runtime memory patching. A
`tplayx.dll` string `zInitCode_CoreExePatching` names that mechanism outright.

**The exe was patched, not rebuilt** [VERIFIED]: `TotalA.exe`'s PE timestamp is still
`Thu Jul 30 14:22:29 1998`, linker 5.10, relocations stripped, 5 sections, ImageBase
`0x00400000`. Section layout: `.text` 0x0FA92A @ VA 0x401000, `.rdata` @ 0x4FC000,
`.data` @ 0x501000, `.tls`, `.rsrc`. A recompile would have reset the timestamp.

**Config keys read by `tdraw.dll`** [VERIFIED, from its string table]: `UnitLimit`, `SfxLimit`,
`UnitType`, `WeaponType`, `AISearchMapEntries`, `X_CompositeBuf`, `Y_CompositeBuf`,
`MultiGameWeapon`, `FullScreenMinimap`, `WheelZoom`, `MenuResolution`, `DoubleClick`,
`ShareDialogExpand`, `DisplayModeWidth/Height`, and the whole `Megamap*` family. It links a C++
`std::regex` runtime and writes an `ErrorLog.txt`; one string reads "Plz Send
Errorlog.txt(In Your TA Path) And The Replay Tad To XPoy(In TAUniverse Or In TAClub)" —
naming **xpoy** as the author of this component.

## Megamap and headline features

The megamap is the flagship 3.9.02 addition: "a groundbreaking, customizable, and interactive
full-screen map view" [VERIFIED — readme]. It is a full-screen replacement for the minimap that
remains **fully interactive**: "Anything you can do in the main game view, you can also do on
the megamap" — select and control units, build structures including line-building, and inspect
LOS/radar/sonar/weapon ranges. Holding SHIFT over an armed unit draws per-weapon range rings.

Implementation, per the available evidence: it lives **inside `tdraw.dll`**, the DirectDraw
replacement. That component owns the framebuffer and the input path, which is what makes a
second full-screen render of the map tractable without touching the exe. Corroborating this,
every megamap key (`MegaMapKey`, `WheelZoom`, `WheelMoveMegaMap`, `MegamapFpsLimit`,
`MegaMapConfig`, the per-sensor `Megamap*Minimum` rings, `MegaMapEmptyRegionColor`) is read by
`tdraw.dll` and by nothing else [VERIFIED]. Defaults: toggle key `MegaMapKey=115` (VK_F4),
icon config `.\Icon\iconcfg.ini`, `MegamapFpsLimit=1000`.

**Mousewheel zoom** is the default entry gesture: `WheelZoom=TRUE`, with `WheelMoveMegaMap=TRUE`
meaning wheel-up zooms to the cursor location (FALSE returns to the previous camera position).
`DoubleClickMoveMegamap` (default FALSE) adds double-click-to-zoom on megamap terrain.

A hard constraint [VERIFIED — readme "Known Issues"]: the megamap requires fullscreen. In
windowed mode "the megamap, whiteboard, line building, and multiplayer ally resource bars won't
be available", and the INI warns "Windowed mode will disable many TA v3.9.02 features". On 2013
AMD/ATI cards and Windows 8 where fullscreen failed, users were left with no megamap. This is
precisely the gap the later cnc-ddraw drop-in packages address.

## Multiplayer compatibility

There is **no version handshake that blocks mixed games**; instead the patch adds reporting
commands and keeps sim-visible changes off by default [VERIFIED — `tplayx.dll` strings +
readme].

- `.report` — "All recorders in the game report their presence, version and status of the
  toggles"; the readme adds that it "now shows the installed patch version of each player". A
  string `uses TA Unofficial Patch` is the reply text. `.ehareport` is the silent self-only
  variant; `.hookreport` reports TA Hook presence.
- `.reportmod` — "All players in the game report their game mod versions."
- `EmitBuggyVersionWarnings` — an internal toggle for warning about known-bad peer versions.
- `.syncon` / `.syncoff` lock game speed to a range to prevent speedjacking.
- `.tad` recordings are stamped with the patch version they were recorded with.

The one genuinely wire-incompatible change is the **weapon ID limit**, which alters packet
structure. It is therefore shipped disabled for multiplayer: `MultiGameWeapon = FALSE`, with
the INI noting "Weapon ID limit increase not yet compatible with Replayer" and the readme
explaining the Replayer "still needs to be updated to correctly interpret the new packet
structure while at the same time maintaining backwards-compatibility with old .tad recordings"
[VERIFIED]. Practically: unpatched and patched clients can share a game because the raised
limits that matter are either client-local (SFX, model buffer, sound mixing, resolution,
megamap) or must be agreed via lobby settings (unit limit); the weapon-ID change is gated off
precisely because it would break both peers and the replay format.

## Relationship to TADR

**The premise needs correcting.** TADR is not the source of `tdraw.dll` — it is the source of
**`tplayx.dll`** [VERIFIED]. That DLL is a Delphi/Pascal build carrying its original SVN source
paths in the binary:

```
F:\TA\TADR\SVN\trunk\src\logging.pas
F:\TA\TADR\SVN\trunk\src\Recorder\idplay.pas
F:\TA\TADR\SVN\trunk\src\Recorder\PluginEngine.pas
F:\TA\TADR\SVN\trunk\src\Recorder\TAMem\SynCommons.pas
F:\TA\TADR\SVN\trunk\src\Recorder\server_commands.inc
```

TADR is the **TA Demo Recorder** — the DirectPlay-proxy lineage descending from the Swedish
Yankspankers' TA Demo, via Xon's 1.0.0.545 `dplayx.dll`. Its version resource reads
"TA Unofficial Patch - Demo Recorder Component / Swedish Yankspankers & TA Universe community",
its internal module name is still `Dplayx.dll`, and a credit string reads
**"Updated by Xon, Xpoy and Rime"** [VERIFIED]. So 3.9.02 is the *distribution*, and TADR is the
upstream source project for its **networking/recording** component specifically.

`tdraw.dll` has a separate lineage: it descends from **xpoy's "TA Interface Upgrade"
`ddraw.dll`**, which the readme lists among the packages this patch supersedes [VERIFIED]. It
is C++ (std::regex, MSVC runtime), unlike TADR's Delphi. The 2022 successor build I inspected
is credited to "vThaldren Developments" with OriginalFilename `idraw.dll` — evidence the
DirectDraw component kept evolving independently after the patch line itself stopped at 3.9.02.

## Sources

- `https://files.tauniverse.com/files/ta/unofficial-patch/` — open Apache index (no login);
  `TA_Patch_3902.exe`, `TA_Patch_Resources.exe`, `old-versions/TA_Patch_3901.exe`.
- `http://patch.tauniverse.com/ta-patch/TA_Patch_Updater.txt` — live auto-updater manifest.
- `readmepatch.txt` — the official 3.9.02 readme, extracted verbatim as RTF from inside
  `TA_Patch_3902.exe` at file offset `0x3F0098`; contains Introduction, Features and full
  Version History. This is the primary changelog record.
- `https://github.com/Skirmisher/TA-Patch-Installers` (archived 2021-10-29) — NSIS installer
  sources; `Patch/notes.txt` gives the supersession checklist and `modstool.exe` invocations.
- `https://github.com/gammata/TA-Unofficial-Patch-Install` — modern cnc-ddraw drop-in packages
  (OTA/ProTA/Escalation/Mayhem/Twilight/Zero); source of the patched binaries analysed here.
- Direct static analysis of `TotalA.exe`, `tdraw.dll`, `tplayx.dll`, `tmusi.dll` (objdump PE
  headers/imports/exports, custom Python PE export parser, `strings`, targeted disassembly).
- `tauniverse.com/forum` was **not** consulted — members-only since 2025-11-18.
