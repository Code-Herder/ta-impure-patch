# D. Quality of life — evidence: the TADR the mods bundle

A read-only pass made 2026-10-04 over the five supported mod installs as the compatibility suite
pins them, asking which TADR each one actually runs and which of TADR's older conveniences it
switches on. Every claim concerns those installs **without Impure** (the
[takeover](../compat/takeover.md) suppresses TADR's initialization) and with fresh TADR
preferences. A player's saved registry options can override any default given here. It is one of
three passes behind [the coverage table](quality-of-life.md), beside
[what Impure has](quality-of-life-impure.md) and [newer TADR builds](quality-of-life-newer.md).

Setup layering is defined in `tools/compat/setups.json`, fixture provenance and hashes in
`tools/compat/fixtures.json` ([setups and outcomes](../compat/setups.md)). `TACOMPAT_CACHE` is the
fixture cache `tacompat.py` uses; every file was read as `${TACOMPAT_CACHE}/fixtures/<fixture>/<file>`,
and no game was launched.

## Which TADR does each supported mod run?

The five supported installs carry very different TADR generations.

| Effective mod | Shipped/effective tdraw | PE file/product version | SHA-256 |
|---|---|---|---|
| Escalation GOLD 10.2.0 | `escalation-gold-10.2.0/TAESC.dll` | 2026.8.6.0 | `61783acaa51ae5348e91ca57706540cff39cbf9905ba6fb26020603d394965df` |
| Total Mayhem 11.3.0 | `mayhem-11.3.0/tdraw.dll` | 2024.3.31.0 | `b8ea362e1bf3e47f00b43e929cd2689746b89338dbb9a7cf0f4b8b6ea2e73a9d` |
| ProTA 4.8 | `prota-4.8/tdraw.dll` | 2025.8.29.0 | `6b46046aa0ea2ab6164ac19cf8c1aba9b1ee219e1123d3fb359615830a859e73` |
| TA Zero Alpha 5 + 120526 update | `tadr-2025.4.24-zero/zdraw.dll`, after the base/Alpha 5/update layering | 2025.4.24.0 | `abec6e349e2dde031bbe8906ccbbb9d8998b86b719ac0355a17bab15077ff5e3` |
| TA Twilight 2.0 Beta 98 | `tadr-v2026.6.8-full/tdraw.dll`, after the Beta 91/98 drop-in layering | 2026.6.8.0 | `b60b173afb7b84ef2a7603051ad673e5991ad9379d58cdbd59ff54e82fd2b084` |

**How the version was read.** Find the `VS_FIXEDFILEINFO` signature `BD 04 EF FE` in the resource
section and decode the four version DWORDs at `+8..+23`. Most of these DLLs also contain the
signature by coincidence in code; the resource offsets are Escalation `0xBE988`, Mayhem `0x77788`,
ProTA `0x8C588`, Zero `0x7FB88` and Twilight `0xAFB88`. The resource values agree with the
human-readable FileVersion and ProductVersion strings. The upstream source to compare against is
the matching tag — `v2024.3.31`, `v2025.4.24`, `v2025.8.29`, `v2026.6.8`, `v2026.8.6` — not current
HEAD ([TADR's tags](https://github.com/tanvanman/TADR/tags)).

A version resource identifies the release generation; source availability alone proves nothing
about a binary. Where a compile-time or runtime gate matters, the rows below were checked against
targeted binary strings or immediates and the shipped loader INIs.

**What this section does not show.**

- It is offline inspection, not live behaviour. A *Yes* below means bundled code, an active
  construction or config gate and a usable input path. It does not report player use, or prove
  every old implementation works correctly.
- Gammata's OTA drop-in is a patch distribution, not one of the five mods. Its tdraw is 5.0.0.0,
  SHA-256 `9ce51279fa236ee35b6d33a626548e313361e16f4bc5a8d2b063e0716dd1d0d7`; its old, private
  provenance was not used to infer anything here.

## Which older conveniences do those installs switch on?

Snapping, queue dragging, reclaim-only patrol, construction kickout and the major panels genuinely
shipped. The stay/scatter guarding options and Unicode support need an extra choice by the player:
finding the module in the DLL does not make those behaviours the default.

Abbreviations: **E** Escalation, **M** Mayhem, **P** ProTA, **Z** Zero, **T** Twilight. *Mod enabled*
uses fresh shipped or default options; an input feature still needs its normal gesture.

| Player-facing feature | Bundled TADR includes a usable feature? | Mod enabled? | Which mods, which defaults | Source and gate evidence |
|---|---|---|---|---|
| Snap metal extractors and geothermal plants to metal/geo spots | M, P, T. E explicitly excludes mex snap; Z has the code but its availability gate is zero | Yes | M, P, T; fresh default radius 3 | Old `tahook.cpp` reads the default and maximum from VA `0x50390A/B`; the M and P loaders write 3 to both. T's 2026.6.8 full config compiles in 3/3; E's 2026.8.6 Escalation config 0/0. [2024 snap functions](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/tahook.cpp#L1740), [2026 full config](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/config_full.h#L8), [Escalation config](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/config_escalation.h#L8) |
| Snap the reclaim cursor to wrecks and reclaimable features | E, M, P, T. Z has dormant code with its availability gate zero | Yes | E, M, P, T; fresh default radius 1 | The same split: old code reads VA `0x503912/13`, and the M and P loaders set 1/1; modern T and E compile in 1/1. [Old code](https://github.com/tanvanman/TADR/blob/v2025.4.24/src/DDraw/tahook.cpp#L1769) |
| Drag queued build/move orders to new positions | All five | Yes | E, M, P, Z, T | Present by v2024.3.31: `tahook.cpp:527..623` primes and performs the drag, and `DragUnitOrders` (`:1692`) updates the order. `iddrawsurface.cpp:177` constructs TA Hook in the oldest release, and modern E and T enable it. [Old queue drag](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/tahook.cpp#L527), [user instructions](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/tdraw.txt) |
| Construction patrol: Hold Position means reclaim only | All five | Yes | E, M, P, Z, T | The oldest source's `TABugFix.cpp:133..177` bypasses build and repair under Hold Position, and its ground and air hook constructors are active (`:562`, `:564`). The old roam/assist-only hook is commented out, so old M and Z are not fully configurable. P, E and T default to reclaim-only for Hold and both for Maneuver and Roam. [2024 patrol](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/TABugFix.cpp#L133), [modern defaults](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/dialog.cpp#L76) |
| Construction patrol: configurable assist-only mode | E, P, T; absent M and Z | No by default | Offered by E, P and T through Ctrl-F2; Hold defaults to reclaim-only, Maneuver and Roam to both | `dialog.cpp` carries the three labels `Reclaim Only`, `Both`, `Assist Only` and their defaults. [ProTA's release defaults](https://github.com/tanvanman/TADR/blob/v2025.8.29/src/DDraw/dialog.cpp#L60), [modern defaults](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/dialog.cpp#L77) |
| Constructors assisting a factory stay put, or scatter, after each unit | E, P, T; absent M and Z | No by default | Offered by E, P and T, but all three movement settings default to **Cavedog** | P's source, from before the config split, reads the gate at VA `0x50391F` (exe file offset `0x101F1F`), and its loader sets it to 1. E and T compile in `FIXED_POSN_GUARDING_CONS_ENABLE=1` in place of the gate. Every choice defaults to CAVEDOG in `dialog.cpp`. [P's gate and installation](https://github.com/tanvanman/TADR/blob/v2025.8.29/src/DDraw/TABugFix.cpp#L747), [P's default options](https://github.com/tanvanman/TADR/blob/v2025.8.29/src/DDraw/dialog.cpp#L71), [T's constructor gate](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/TABugFix.cpp#L1484) |
| Queue buildings beneath your own mobile units, moving blockers out of the way | E, P, T; absent M and Z | Yes | E, P, T | Introduced 2025.7.12; limited to your own units 2025.8.19. P constructs six kickout and build-under hooks unconditionally; E and T enable the module by config. The target-site immediates `0x47D554` and `0x403CD0` occur once each in the E, P and T binaries and not at all in M and Z. [P's constructor](https://github.com/tanvanman/TADR/blob/v2025.8.29/src/DDraw/ConstructionKickout.cpp#L83), [release history](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/tdraw.txt) |
| Game-time clock and wind/tidal readouts in the resource HUD | All five include the code and call it | Yes, by code and defaults | E, M, P, Z, T; Z's old bundle still shows wind and tidal, unlike the current dedicated Zero builds | The oldest `iddrawsurface.cpp:613` and `:724` call BlitWeatherReport; the old `cinomce.cpp:855..929` prints Wind, Tidal and Game Time. E's and T's master weather flag is 1. Z's bundle predates the 2026.7.27 config that hides wind and tidal. [Old call](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/iddrawsurface.cpp#L613), [old renderer](https://github.com/tanvanman/TADR/blob/v2025.4.24/src/DDraw/cinomce.cpp#L855), [the new Zero build's release note](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/tdraw.txt) |
| Map-owned dragon's teeth and fort walls visible regardless of the player's LOS | All five include the rendering hook, dormant or active; only P is shown to opt in | Yes | P only | `DrawPlayer11DTProc` bypasses LOS for owner index 11 only, not stock's 10. The three marker patches stay commented out in every inspected upstream tag. P's loader writes `0B` at file offsets `0x82E75`, `0x82ED3` and `0x82F31`; M's loader lacks those rows; the E, Z and T exes keep `0A` at all three, and no TADR marker-patch constructor is active. `VISIBLE_MAP_DTS=1` in a config does **not** by itself enable the behaviour. [Owner marker and rendering hook](https://github.com/tanvanman/TADR/blob/v2025.8.29/src/DDraw/TABugFix.cpp#L108), [marker patches left out](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/TABugFix.cpp#L1413) |
| Strategic megamap with wheel entry and exit, and same-type double-click selection | All five | Yes | E, M, P, Z, T | Every shipped INI sets `FullScreenMinimap`, `WheelZoom`, `WheelMoveMegaMap` and `DoubleClick` TRUE and `DoubleClickMoveMegamap` FALSE; the line positions are in the INI table below. The module exists in the oldest release. [Old module](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/fullscreenminimap.cpp), [2026 config](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/config_full.h) |
| TA Hook line and ring placement, optimized DT spacing and auto-click | All five | Yes | E, M, P, Z, T; the gesture key is X unless remapped | The old dialog's fresh defaults are OptimizeDT on, FullRings on, key code 88 (X), delay 10. The module is constructed in the oldest release and enabled in E and T. Mayhem's shipped settings agree: `OptimizeDTEnabled=1`, `FullRingsEnabled=1`, `BuildLinesRingsKey=88`. [Old defaults](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/dialog.cpp#L1035), [old placement](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/tahook.cpp#L915) |
| Whiteboard markers and freehand drawings shared with allies | All five | Yes | E, M, P, Z, T | The oldest `iddrawsurface.cpp:175` constructs AlliesWhiteboard; E and T set `USEWHITEBOARD=1`. The recorder half sends the marker messages ([the recorder's data](../tadr-merge-exploration.md#d-data-the-recorder-feeds-tdraws-interface)). [Old construction](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/iddrawsurface.cpp#L175), [whiteboard](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/whiteboard.cpp) |
| Unicode / non-Latin chat font | All five contain the feature | No by default | None of the effective shipped INIs names a UnicodeSupport font | `ddraw.cpp` constructs UnicodeSupport only when the INI's `UnicodeSupport` names a font, and leaves the pointer NULL otherwise. There is no UnicodeSupport row in E's `TAESC.ini`, M's `mayhem.ini`, P's `ProTA.ini`, Z's `TAZero.ini`, or T's `TA.ini` and `totala.ini`. [Old activation](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/ddraw.cpp#L102), [modern activation](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/ddraw.cpp#L121) |
| Allies' resource/income panel | All five | Yes, in multiplayer with an ally | E, M, P, Z, T | The oldest source constructs CIncome at `iddrawsurface.cpp:176`; `cinomce.cpp` shows AllyIncome (every player, to a watcher or a replay). Its data arrives in recorder packets `0x28` and `0x16`, so a port needs the recorder half too. [Old construction](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/iddrawsurface.cpp#L176), [modern panel](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/cinomce.cpp#L109), [the recorder's data](../tadr-merge-exploration.md#d-data-the-recorder-feeds-tdraws-interface) |
| Allies' camera rectangles on the minimap, and camera sharing | All five, as the tdraw and recorder pair | Yes | All five; the recorder's `.sharemappos` is on by default | The recorder appends camera data when it changes, and tdraw's maprect draws allies' views. This is network data as well. [The recorder's data](../tadr-merge-exploration.md#d-data-the-recorder-feeds-tdraws-interface), [old maprect](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/maprect.cpp) |
| Keyboard selection of idle constructors, idle factories and combat units | All five | Yes | E, M, P, Z, T | ExternQuickKey is installed when DirectDraw initializes. Old source: Ctrl-B `FindIdleConst`, Ctrl-F `FindIdelFactory`, Ctrl-S combat selection, and the same-type selection helpers. [Old installed module](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/ddraw.cpp#L114), [input handlers](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/ExternQuickKey.cpp#L209) |
| F11's configurable sharing/command macro | All five | Yes | E, M, P, Z, T; active on F11, with a non-empty fresh default | `VK_F11` calls WriteShareMacro in every effective release. The fresh default runs `+setshareenergy 1000`, `+setsharemetal 1000`, `+shareall`, `+shootall`. It is edited in Ctrl-F2's Chat Macro field and saved as the registry value ShareText; no F1Disable or F11-disable gate exists. [Old key handler](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/tahook.cpp#L358), [old default](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/dialog.cpp#L1072), [P's default field](https://github.com/tanvanman/TADR/blob/v2025.8.29/src/DDraw/dialog.cpp#L109), [E's handler](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/tahook.cpp#L364) |
| Follow a recorded player's camera (`.lockon <name>`) | All five native recorder and tdraw pairs | Yes, on request | E, M, P, Z, T; the replay must contain that player's camera data, and following starts when a target is chosen | All five native recorder DLLs contain the standalone `lockon` command name and its replay-follow help text. The command is registered self-only, not host-only, and the source handler sets the shared `lockviewon` during replay. Camera-position recording defaults on in every native binary. [Old source handler](https://github.com/tanvanman/TADR/blob/4df4e0c/src/Recorder/self_commands.inc#L478), [registration](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/Recorder/AddCommands.inc#L189), [camera consumer](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/maprect.cpp#L119) |

**The F11 macro, checked further.** All five native DLLs contain the `ShareText`, `F11`,
`+setshareenergy 1000` and `+shareall` strings, and the old M/Z releases agree with the newer P/E/T
source handlers. The macro does not depend on `ShareDialogExpand`: `WriteShareMacro` splits its
saved text on carriage returns and passes each command to ChatText
([v2024.3.31](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/tahook.cpp#L775),
[v2026.8.6](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/tahook.cpp#L861)). The fresh
registry default exists in all five generations, and the shipped INIs carry no ShareText override
and no F1Disable option. A player's saved ShareText can replace or empty it; that is a preference,
not a distribution default.

**The expanded sharing dialog** also ships with its gate on in all five: E `TAESC.ini:96`,
M `mayhem.ini:105`, P `ProTA.ini:94`, Z `TAZero.ini:71` and T Beta 98 `TA.ini:73` (the final
full-package `totala.ini:82` agrees) all say `ShareDialogExpand=TRUE`. The constructor reads the
option with a TRUE fallback and installs the ShareDialogInit/ShareDialogProc hooks, which add
resource-threshold controls and extra buttons where the matching GUI controls exist
([old gate](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/GuiExpand.cpp#L26),
[old hook installation](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/sharedialog.cpp#L358)).
It is not the F11 menu: F11 runs the macro. Each mod's GUI was not surveyed control by control, so
the coverage table carries only the F11 macro as a fully verified row.

**The replay-follow binaries.** E's `eplayx.dll`, the M and P `tplayx.dll` and T's effective
drop-in `tplayx.dll` are resource version 3.9.2.416; Z's effective base `zplayx.dll` is 3.9.2.0.
Their SHA-256s:

| Recorder | SHA-256 |
|---|---|
| E `eplayx.dll` | `29e9f66d93e788dbb2cee0cb3c9015094c7cdbe1de38a665be3b5fdc9842967b` |
| M `tplayx.dll` | `688d42b4901c1e825db5b1688353db6ecc286ae75c0fe6885a4c42aea054c6b6` |
| P `tplayx.dll` | `b641a7c97389088f93e7270425a24a1b6d0c442a112cd6be47a46c2773efdacf` |
| Z `zplayx.dll` | `44f2d45fefe294110ac5ac7a4dccbc5c1f27f002414b1fb2886b4ec69b092aae` |
| T `tplayx.dll` | `9ccc64d8305163553cd06e9328eb6e655486b27c3f8cc37f460b7f96f1be7f72` |

All five contain the exact standalone `lockon` and `sharemappos` command strings and the same
replay-follow description, and no effective shipped INI disables either. Disassembly confirms that
camera sharing's registry default is true: the four 3.9.2.416 recorders push 1 at VA `0x738334` and
then pass `sharepos` (VA `0x7389FC`) to the options reader; Zero pushes 1 at `0x734089` and passes
`sharepos` at `0x7344FC`. A player can turn `.sharemappos` off while recording, and following
needs the camera data to be in the replay. No host gate governs the local follow command. This is
an availability and activation check, not a new replay test.

**The loader INIs.** The Patch Loader INIs embedded in the shipped `dplayx.dll`s were decoded
read-only from their ASCII resources; line numbers count from `[Settings]`.

- Mayhem 11.3.0: lines 573..579, `;Mex snap settings` `[00101F0A]` `patch=03 03`, and
  `;wreck snap settings` `[00101F12]` `patch=01 01`.
- ProTA 4.8: mex `patch=03 03` at lines 454..455; wreck `patch=01 01` at 463..464; the guarding
  gate `[00101F1F] patch=01` at 544..545; the map-owner markers `[00082E75]`, `[00082ED3]` and
  `[00082F31]`, each `patch=0B`, at 439..446.
- The loader's section names are raw file offsets, not VAs
  ([the loader's offsets](../deep-patch-loader.md#the-named-patch-corpus-full-table-name-address-effect)).
  For `.data` it adds `0x401A00`, so file offset `0x101F1F` is VA `0x50391F` — not VA `0x501F1F`.

**Where the shipped INIs set the megamap and double-click defaults** (line numbers):

| Config | Same-type `DoubleClick` | `FullScreenMinimap` | `WheelZoom` / `WheelMoveMegaMap` | `DoubleClickMoveMegamap` |
|---|---:|---:|---:|---:|
| E `TAESC.ini` | 89 TRUE | 113 TRUE | 161 / 168 TRUE | 175 FALSE |
| M `mayhem.ini` | 98 TRUE | 122 TRUE | 141 / 148 TRUE | 155 FALSE |
| P `ProTA.ini` | 87 TRUE | 112 TRUE | 126 / 133 TRUE | 140 FALSE |
| Z `TAZero.ini` | 64 TRUE | 88 TRUE | 103 / 110 TRUE | 117 FALSE |
| T Beta 98 `TA.ini` | 66 TRUE | 90 TRUE | 109 / 116 TRUE | 123 FALSE |

**How the rows were decided.**

- "Bundled TADR includes a usable feature" treats a zero maximum mex or wreck radius as
  unavailable, even where dormant snapping code exists — Ctrl-F2 shows such a feature as NA.
  Otherwise code that ships but cannot be used would be called enabled.
- The panel, whiteboard and camera rows work with the bundled recorder. They say nothing about
  whether Impure has the matching presentation or network messages.
- Impure already has some of these input conveniences in part;
  [what Impure has](quality-of-life-impure.md) marks those.

**What this section does not show.**

- The old weather renderer tests the ally panel's `SurfaceMemory == NULL` instead of the back
  buffer it is handed; release 2026.6.8 fixes that. So the old M/P/Z defaults include and call the
  clock and weather code, but whether every skirmish HUD visibly draws it was not checked. This is
  not a live reproduction.
- Map-DT enabling was read from the executables' marker bytes and the loader INIs; no game run
  showed what it looks like.
- A player's saved registry preferences can override every default, and no usage telemetry was
  collected.

## Which entries are newer source work, not bundled features?

The newest chat refinements postdate every supported mod's bundle. The basic megamap ships
everywhere, but its later fixed-feature rendering and dithering must be kept apart from the base
feature.

- Movable chat positioning and local per-player mute arrived in commit `72018b5`, dated
  2026-08-16 — after v2026.8.6, the latest supported bundle.
  [Commit](https://github.com/tanvanman/TADR/commit/72018b5)
- Channel-separated chat, scrollback history and the enlarged, coloured and outlined font arrived
  in `3bed4eb`, 2026-09-05. No E/M/P/Z/T bundle contains the `ChatPosition` or `ChatHistory`
  preference strings, and the source files do not exist at their release tags.
  [Commit](https://github.com/tanvanman/TADR/commit/3bed4eb)
- Fixed map-feature rendering on the megamap was released 2026.6.23: supported E (v2026.8.6) has
  it; T (v2026.6.8) and the older M, P and Z do not. Dithering was released 2026.7.27 and the
  preserved enhanced minimap colours 2026.8.1; E has both, the other four do not.
  [Tagged release history](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/tdraw.txt)
- The dedicated Zero build that omits the wind and tidal readouts was released 2026.7.27. It is
  not the pinned Zero install's 2025.4.24 DLL.
  [Release history](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/tdraw.txt)

So the coverage table marks movable chat and chat history **No** in every column for the supported
fixtures. That says nothing about whether upstream released them later; that question, and team
nanolathe, reload bars, counters, rotation, ghost preview, accessible chat and allied queues, are
[the newer-builds pass](quality-of-life-newer.md)'s.
