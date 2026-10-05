# D. Quality-of-life coverage in supported mods

Several conveniences enabled by native supported mods remain missing from Impure: placement and
reclaim snapping, editing queued orders, line/ring construction, construction behavior controls,
and allied information. Other gaps require a preference change, or belong to newer TADR builds
that none of those mods bundles. **Rotation support ships with Escalation and Twilight, but no
checked mod enables rotation in its unit definitions.**

Checked **2026-10-04** against Impure source, native DLLs, public release assets, loader settings
and mod archives. This is an availability/defaults audit, not player-usage telemetry or a new
gameplay test. The [compatibility setups](../compat/setups.md) define the supported installations;
`tools/compat/fixtures.json` pins their files and `tools/compat/setups.json` defines their layers.
An optional player-installed TADR upgrade is a different installation.

**Mod enabled = Yes** means at least one supported native distribution enables the feature
through shipped content/settings or fresh defaults. A normal keypress or gesture counts as
enabled. **No by default** means an available feature needs an extra preference change. Saved
registry preferences can override those defaults. These are behaviors **without Impure**: the
[takeover](../compat/takeover.md) suppresses TADR and recorder initialization, so bundled support
does not carry through automatically.

## Native bundled versions

| Code | Supported mod version checked | Effective bundled TADR | DLL / bytes |
|---|---|---|---|
| E | Escalation GOLD 10.2.0 | [v2026.8.6, Escalation build](https://github.com/tanvanman/TADR/releases/tag/v2026.8.6) | `TAESC.dll` / 908288 |
| M | Total Mayhem 11.3.0 | [v2024.3.31](https://github.com/tanvanman/TADR/releases/tag/v2024.3.31) | `tdraw.dll` / 628224 |
| P | ProTA 4.8 | [v2025.8.29](https://github.com/tanvanman/TADR/releases/tag/v2025.8.29) | `tdraw.dll` / 716800 |
| Z | TA Zero Alpha 5 + TAZ31 120526 update | [2025.4.24, Zero-specific TAF build](https://content.taforever.com/featured_mod_dropins/tdraw-2025.4.24-Zero.zip) | `zdraw.dll` / 663040 |
| T | TA Twilight 2.0 Beta 98, including effective drop-in layers | [v2026.6.8, full build](https://github.com/tanvanman/TADR/releases/tag/v2026.6.8) | `tdraw.dll` / 865280 |

E/M/P were compared byte for byte with their official release assets. T's Beta 98 DLL and final
TAF full-build layer are identical. Z is identified by its pinned TAF package and version; it is
not claimed identical to the generic upstream DLL. Measured DLL SHA-256 values:

| Code | SHA-256 |
|---|---|
| E | `61783acaa51ae5348e91ca57706540cff39cbf9905ba6fb26020603d394965df` |
| M | `b8ea362e1bf3e47f00b43e929cd2689746b89338dbb9a7cf0f4b8b6ea2e73a9d` |
| P | `6b46046aa0ea2ab6164ac19cf8c1aba9b1ee219e1123d3fb359615830a859e73` |
| Z | `abec6e349e2dde031bbe8906ccbbb9d8998b86b719ac0355a17bab15077ff5e3` |
| T | `b60b173afb7b84ef2a7603051ad673e5991ad9379d58cdbd59ff54e82fd2b084` |

## Missing or partial coverage

**All** means E/M/P/Z/T. Existing Impure build ghosts, `PreviewPieces=`, health/group/order/range
markers, wheel world zoom, HUD scaling and replay playback/seeking are already implemented and
are excluded. The preview row names only the unsupported extensions. The separate megamap is
listed as a deliberate port exclusion, not planned work.

| Feature missing from Impure | Mod enabled? | Enabled mods / condition | Included in bundled TADR? |
|---|---|---|---|
| Metal/geo placement snapping | **Yes** | M/P/T | Code present in M/P/Z/T; Z's gate disables it; E build disables support |
| Reclaim cursor snapping to wrecks/features | **Yes** | E/M/P/T | Code present in all five; Z's gate disables it |
| Drag queued build/move orders to edit positions | **Yes** | All | All |
| Line/ring building, spacing and optimized DT rows | **Yes** | All; TA Hook placement gesture | All |
| Reclaim-only construction patrol on Hold Position | **Yes** | All | All |
| Configurable assist-only construction patrol | **No by default** | E/P/T offer it; defaults are reclaim-only or both | E/P/T; absent M/Z |
| Constructors stay put/scatter while guarding factories | **No by default** | E/P/T offer choices; defaults remain Cavedog behavior | E/P/T; absent M/Z |
| Build beneath own mobile units; automatically move blockers | **Yes** | E/P/T | E/P/T; absent M/Z |
| Live game-time clock and wind/tidal HUD | **Yes** † | All, by code/defaults; older rendering caveat below | All; bundled Z predates later wind/tidal removal |
| Map-owned DTs/fort walls visible independently of LOS | **Yes** | P only; loader supplies required owner markers | All contain the hook; only P's bundle makes it effective |
| Shared whiteboard drawings and labelled map markers | **Yes** | All, with allies | All, including the bundled recorder transport |
| Allies' minimap camera rectangles / camera sharing | **Yes** | All; `.sharemappos` defaults on | All, including the bundled recorder transport |
| Follow another player's recorded camera during replay (`.lockon <name>`) | **Yes** | All; choose a player whose camera data exists in the recording | All native recorder/TADR pairings |
| Allies' live resource/income panel | **Yes** | All, in multiplayer with allies | All, including the bundled recorder transport |
| Enhanced idle/category selection hotkeys and same-type double click | **Yes** | All; TADR enhancements to stock selection | All |
| F11 configurable sharing/command macro | **Yes** | All; nonempty default macro runs on F11 | All |
| Separate strategic megamap view — intentionally excluded from the port | **Yes** | All; Impure already has wheel world zoom | All; E alone has the later fixed-feature/dithering enhancements |
| Building/factory placement rotation and directional build-menu choices | **No** | No mod has active `Rotations=` definitions | E/T; absent M/P/Z |
| Extra build-preview keys — **partial**: `PreviewObject3D`, `PreviewPiecesS/E/N/W`, `PreviewFaceOpponent` | **Yes** | E uses `PreviewFaceOpponent=1` in 12 units; ordinary ghosts and `PreviewPieces=` already work in Impure | E/T contain the preview extensions; absent M/P/Z |
| Accessible Chat backdrop and legible chat font | **No by default** | E has the option, default off; no shipped override | E only |
| Unicode/non-Latin chat text/input support | **No by default** | No shipped config supplies the required Unicode font | All contain the feature |
| Weapon reload-progress bars (`reloadbar=1`) | **No** | No active content opt-ins, and no bundled module | No, in any of the five |
| Stockpile and loaded-transport overhead counters | **No** | Supporting module absent | No, in any of the five |
| Team-coloured build/reclaim nanolathe streams and nanoframes | **No** | Supporting module absent; distinct from existing team-coloured models | No, in any of the five |
| Allies' unstarted build-queue overlays | **No** | Supporting module absent | No, in any of the five |
| Movable/extended chat: anchor, channels, scrollback and font controls | **No** | Supporting modules absent | No, in any of the five |
| Local per-player `.mute` / `.unmute` for chat, pings and drawings | **No** | Supporting module absent | No, in any of the five |
| Percentage resource-sharing thresholds, e.g. `+setshareenergy 50%` | **No** | Supporting module absent | No, in any of the five |

† M/P/Z include and invoke the old clock/weather renderer, but it tests the ally panel's
`SurfaceMemory` instead of the supplied backbuffer. That guard was corrected in v2026.6.8.
Visible output in every game mode was not tested here. See the
[old renderer](https://github.com/tanvanman/TADR/blob/v2025.4.24/src/DDraw/cinomce.cpp#L855).

This inventory covers player conveniences. Separate lobby/game-control features, security,
simulation fixes and limits remain in their [port groups](overview.md). Inclusion here does not
override a feature's earlier safety decision: `PreviewObject3D` is parked and the
[opponent-facing preview](data-keys-evidence.md#4-previewfaceopponent) has unresolved fog and
commander-identity concerns. Group D has a survey; the first feature's
[resource-snapping, Quick Build and mod-profile design](resource-snapping.md) is agreed, with
implementation and live placement verification pending. It adds a separate BAR-style contextual
mex/geo toggle, owned completed-building upgrades and queued-site indicators. Proposed defaults
enable both resource features for Stock TA, M/P/T and lock them off for E/Z and unidentified mods.
Those are Impure choices; this table continues to audit native bundled TADR behavior. Quick Build
is additional planned work and is not evidence of another feature in any bundled TADR DLL.

## Activation evidence for older conveniences

The historical releases use spare exe bytes as configuration; newer build configurations use
compile-time gates. Reading only current `config_*.h` would misclassify old native bundles.
The [engine map](../exe-reverse-engineering.md#tadr-qol-configuration-bytes-and-map-owner-markers)
records the raw-file/VA mapping and owner-marker disassembly.

| Feature family | Evidence checked |
|---|---|
| Placement/reclaim snapping | [v2024.3.31 `tahook.cpp`](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/tahook.cpp#L1740) reads default/max bytes at `0x50390A/B` and `0x503912/13`. M/P embedded loader INIs write `03 03` and `01 01` at file offsets `0x101F0A` and `0x101F12`; Z leaves the availability gates zero. [T full config](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/config_full.h#L8) supplies 3/3 and 1/1; [E config](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/config_escalation.h#L8) supplies 0/0 and 1/1. |
| Queue editing and line/ring construction | Oldest inspected release constructs TA Hook. [Drag handlers](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/tahook.cpp#L527) and [placement code](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/tahook.cpp#L915) are present; fresh dialog defaults enable full rings and optimized DT spacing, with X as the gesture key. |
| Patrol and guarding constructors | [Old patrol hook](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/TABugFix.cpp#L133) implements Hold Position reclaim-only; its assist-only path is commented out. [P dialog defaults](https://github.com/tanvanman/TADR/blob/v2025.8.29/src/DDraw/dialog.cpp#L60) offer all three patrol modes and default guarding to Cavedog. P loader enables the guarding gate at raw file `0x101F1F` / VA `0x50391F`; E/T compile it in. |
| Building under own units / blocker kickout | [P constructor](https://github.com/tanvanman/TADR/blob/v2025.8.29/src/DDraw/ConstructionKickout.cpp#L83) installs it; E/T compile it in. Native E/P/T contain the install-site immediates; M/Z do not. |
| Map-owned DT visibility | [Owner-11 draw hook](https://github.com/tanvanman/TADR/blob/v2025.8.29/src/DDraw/TABugFix.cpp#L108) needs map-owned units marked 11. Upstream's three marker writes are commented out. Only P's loader writes `0B` at file `0x82E75`, `0x82ED3`, `0x82F31`; M lacks those patches and E/Z/T exes retain `0A`. A compiled draw hook alone does not enable the behavior. |
| Selection and F11 | [Installed selection module](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/ddraw.cpp#L114) and [input handlers](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/ExternQuickKey.cpp#L209) precede every native bundle. [F11 handler](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/tahook.cpp#L358) runs `WriteShareMacro`; the [fresh default](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/dialog.cpp#L1072) runs `+setshareenergy 1000`, `+setsharemetal 1000`, `+shareall`, `+shootall`. All five native DLLs contain the macro/default strings; shipped INIs have no override. Registry `ShareText` can replace or empty it. |
| Megamap and double click | All effective native INIs set `FullScreenMinimap`, `WheelZoom`, `WheelMoveMegaMap` and `DoubleClick` TRUE. `DoubleClickMoveMegamap` is FALSE. Base support is present in the [old module](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/fullscreenminimap.cpp). |
| Unicode | [Activation](https://github.com/tanvanman/TADR/blob/v2024.3.31/src/DDraw/ddraw.cpp#L102) requires a nonempty `UnicodeSupport` font name. None of the five effective native configs supplies it. |

F11 runs a macro; it does not open the sharing dialog. `ShareDialogExpand=TRUE` is separately
shipped by all five distributions, but each mod's individual expanded controls were not audited,
so that is not a fully verified missing-feature row.

Whiteboard, camera rectangles, allied income and `.lockon` require the recorder as well as tdraw
([their shared-data paths](../tadr-merge-exploration.md#d-data-the-recorder-feeds-tdraws-interface)).
The native E/M/P/T recorders identify as 3.9.2.416, Z as 3.9.2.0. All contain standalone `lockon`
and `sharemappos` commands and replay-follow help. Native disassembly confirmed camera-sharing's
fresh registry default is true. The [local replay command](https://github.com/tanvanman/TADR/blob/4df4e0c/src/Recorder/self_commands.inc#L478)
has no host-only gate; [tdraw consumes its target](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/maprect.cpp#L119).
Following requires the recording to contain that player's camera data; `.sharemappos` can be
disabled by the recorder's user.

## Rotation and preview content census

Rotation, directional build-menu choices and model placement previews first appear in stable
[v2026.5.9](https://github.com/tanvanman/TADR/releases/tag/v2026.5.9). E/T ship later versions;
M/P/Z predate them. The E/T build-menu rotation checkbox defaults on, but the menu choices need
active `Rotations=` unit definitions. Accessible Chat arrives in
[v2026.7.3](https://github.com/tanvanman/TADR/releases/tag/v2026.7.3), so only E contains it;
`ChatBackdrop` defaults off. No shipped config overrides either checkbox.

Every nonempty fixture archive was read with `tools/hpipack.py`; `.fbi`, `.tdf` and `.ini`
assignments were scanned case-insensitively after removing block and line comments while
preserving semicolon assignment terminators. Loose configs and Zero/Twilight override layers
were checked too. These are archive record counts, not effective merged unit counts. Seven
zero-length Twilight Beta 91 archives were confirmed empty and skipped. No parse errors occurred.

| Archive layer | FBI records | TDF records | Active `Rotations` | Active `reloadbar` | Active extra preview keys |
|---|---:|---:|---:|---:|---|
| E | 549 | 468 | 0 | 0 | `PreviewFaceOpponent=1` in 12 |
| M | 505 | 195 | 0 | 0 | 0 |
| P | 317 | 469 | 0 | 0 | 0 |
| Z Alpha 5 | 269 | 610 | 0 | 0 | 0 |
| Z TAZ31 120526 override | 269 | 610 | 0 | 0 | 0 |
| T Beta 91 | 505 | 1087 | 0 | 0 | 0 |
| T Beta 98 override | 504 | 791 | 0 | 0 | 0 |

The 12 E unit IDs are `armamb`, `armbrtha`, `armguard`, `armvulc`, `corbuzz`, `corint`, `corpun`,
`cortoast`, `corspun`, `armsbertha`, `cornova`, `corbfg`. `PreviewFaceOpponent` changes the ghost's
facing; it does not enable manual rotation. E's only `Rotations` examples are commented out in
`corgant` and `corint`. Escalation's existing scripted factory-exit directions are also separate
from TADR's `Rotations=` placement feature.

No active `PreviewObject3D` or `PreviewPiecesS/E/N/W` assignments were found. E's shipped
`NanoframePreviewFill=FALSE` selects wireframe previews. None of the loose configs supplies
`TeamColorNanolathe`, `ShowStockpileCounter`, `ShowTransportCounter`, `ChatBackdrop`,
`BuildMenuRotationOverlay`, `ChatAnchor` or `ChatRenderer`. See the
[stable key documentation](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/tdraw.txt)
and [dialog defaults](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/dialog.cpp#L146).

## Newer features: downloadable development builds

The later No/No rows are **not all unbuilt git work**. Fixed public development releases were
downloaded and inspected; supporting source history and installation code were checked as well.
These dates identify verified downloadable snapshots, not necessarily the first transient CI
artifact. None is part of the five native bundles above.

| Feature family | Verified public download | Gate / activation in pinned `dev-dcff5dd` source |
|---|---|---|
| Reload bars | [dev-1615f13](https://github.com/tanvanman/TADR/releases/tag/dev-1615f13), published 2026-08-30 (build label 2026.8.29) | Weapon `reloadbar` low bit must be 1 |
| Stockpile / transport counters | Same August download | Both preferences default TRUE in supporting DLLs; own units with health bars |
| Team-coloured nanolathe | Same August download | `TeamColorNanolathe=FALSE`; requires opt-in |
| Allied queued builds | Same August download contains the feature | Later `ALLIED_BUILD_QUEUE_ENABLE=0` in every inspected configuration; no controls, packets or rendering compiled in then |
| Chat anchor / local player mute | [dev-682ee2f](https://github.com/tanvanman/TADR/releases/tag/dev-682ee2f), published 2026-09-13 | Anchor unset leaves engine position; mute module enabled only in Escalation config, no players initially muted |
| Extended chat renderer | Same September download | `ChatRenderer=engine`, custom renderer optional; split/font/line controls default 0 |
| Percentage sharing | Same September download | Escalation config only; percent command argument activates percent mode |

The August Escalation binary contains the reload/counter/team-nano/queue signatures; installation
calls were checked for reload, counters and team nano. The September Escalation binary contains
chat-anchor/renderer, `.mute` and SharePercent install diagnostics. Source modules are absent
from the [v2026.8.6 tree](https://github.com/tanvanman/TADR/tree/v2026.8.6/src/DDraw).
Defaults/gates above are pinned to **`dev-dcff5dd`** (September 20), not the mutable `latest` tag:
[reload](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/ReloadBars.cpp#L240),
[counters](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/UnitStatusCounters.cpp#L355),
[team nano](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/TeamColorNanolathe.cpp#L158),
[allied-queue gate](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/config.h#L107),
[Escalation gates](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/config_escalation.h#L160),
[chat defaults](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/ChatLayout.cpp#L759),
[percentage sharing](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/SharePercent.cpp#L178).
The development downloads were not gameplay-tested in this audit.

## What Impure already covers, and the remaining boundaries

The checked source is Impure `faabb488`, with feature readers, handlers, settings and draw paths
examined together rather than inferring absence from module names:

- `tagpu_datakeys.c` reads `PreviewPieces`; `tagpu_native.c` renders the masked ghost and default
  heading. It does not read `Rotations`, alternate preview models, per-facing lists or
  `PreviewFaceOpponent`. COB getters 32 and 69–75 are [implemented](cob.md); those script getters
  do not implement the preview-key reader.
- `tagpu_order.c` draws stock SHIFT orders/ranges/queued ghosts; it does not implement dragging,
  snapping, line/ring placement, blocker kickout or configurable construction behavior.
- `tagpu_mark.c` draws health/group markers, without reload bars or stockpile/transport counters.
  Team-coloured models and ghosts are separate from construction/reclaim effect colouring.
- `tagpu_menu.c` exposes renderer/display settings, without the listed TADR options/panels.
  `tagpu_shield.c` handles `WM_CHAR` as a low byte and `tagpu_text.h` uses byte glyphs; the
  Unicode/chat extensions are missing.
- `tagpu_cards.c` provides replay resource cards and Sight. Camera Jump/Follow/Free controls are
  disabled and say that the camera was not recorded. Live allied income, whiteboard and camera
  transport remain missing; current replay playback and seeking are separate implemented work.
- The native map-DT draw path has no owner-11 LOS bypass. Retaining a mod's marker bytes under
  takeover does not reproduce TADR's rendering hook.

Future audits should repeat the fixture identity, native activation and current-Impure checks
together. A new upstream configuration or an optional mod upgrade cannot change this native
coverage table without checking the effective installed DLL and its content/defaults.
