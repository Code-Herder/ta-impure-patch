# D. Quality of life — evidence: newer TADR builds

A read-only pass made 2026-10-04 over TADR's newer conveniences — rotation, placement previews,
accessible chat, reload bars, unit counters, team-coloured nanolathe, allied queues, extended chat,
mute and percentage sharing — asking which of them the supported mods' own TADR DLLs contain, which
the mods switch on, and which exist only in later public downloads. It is one of three passes
behind [the coverage table](quality-of-life.md), beside
[what Impure has](quality-of-life-impure.md) and
[the TADR the mods bundle](quality-of-life-bundles.md), which covers the older conveniences.

The scope is the supported distributions pinned by `tools/compat/setups.json` and
`tools/compat/fixtures.json` ([setups and outcomes](../compat/setups.md)), not a player who
replaces a mod's TADR with a newer download. `TACOMPAT_CACHE` is the compatibility harness's
fixture cache. The measurements read `$TACOMPAT_CACHE/fixtures/<fixture>/<DLL>`, inspect the PE
COFF timestamp and ASCII strings, and compare every byte with official release assets downloaded
into memory. No game was launched.

## Which supported mod bundles contain the features?

Of the supported native distributions, Escalation GOLD 10.2.0 carries stable TADR v2026.8.6 and
Twilight Beta 98 carries stable v2026.6.8. Those two contain building rotation, its directional
build-menu overlays and nanoframe placement previews. Only Escalation contains the later Accessible
Chat option, and it defaults off. Reload bars, the extra unit counters, team-coloured nanolathe,
allied queued builds, movable and extended chat, player mute and percentage sharing are in none of
them ([v2026.8.6 source](https://github.com/tanvanman/TADR/tree/v2026.8.6/src/DDraw),
[v2026.6.8 source](https://github.com/tanvanman/TADR/tree/v2026.6.8/src/DDraw)).

| Supported native distribution | Effective TADR DLL | Exact public release identity | SHA-256 of the measured DLL |
|---|---|---|---|
| Total Mayhem 11.3.0 | `tdraw.dll`, 628224 bytes | Byte-identical to the DLL in the official v2024.3.31 `tdraw-v2024.3.31.zip`; PE timestamp 2024-03-30 22:49:25 UTC | `b8ea362e1bf3e47f00b43e929cd2689746b89338dbb9a7cf0f4b8b6ea2e73a9d` |
| ProTA 4.8 | `tdraw.dll`, 716800 bytes | Byte-identical to the DLL in the official v2025.8.29 `tdraw.zip`; PE timestamp 2025-08-28 23:58:58 UTC | `6b46046aa0ea2ab6164ac19cf8c1aba9b1ee219e1123d3fb359615830a859e73` |
| TA Zero Alpha 5 + TAZ31 120526 | `zdraw.dll`, 663040 bytes | TAF's `tdraw-2025.4.24-Zero.zip`, version as the fixture documents it; PE timestamp 2025-04-23 22:03:53 UTC | `abec6e349e2dde031bbe8906ccbbb9d8998b86b719ac0355a17bab15077ff5e3` |
| Escalation GOLD 10.2.0 | `TAESC.dll`, 908288 bytes | Byte-identical to the v2026.8.6 Escalation release; PE timestamp 2026-08-06 08:59:10 UTC | `61783acaa51ae5348e91ca57706540cff39cbf9905ba6fb26020603d394965df` |
| TA Twilight 2.0 Beta 98 | `tdraw.dll`, 865280 bytes | Beta 98's DLL and the final TAF v2026.6.8-full layer are byte-identical, and the manifest pins both; PE timestamp 2026-06-08 00:04:33 UTC | `b60b173afb7b84ef2a7603051ad673e5991ad9379d58cdbd59ff54e82fd2b084` |

The releases: [Mayhem's](https://github.com/tanvanman/TADR/releases/tag/v2024.3.31),
[ProTA's](https://github.com/tanvanman/TADR/releases/tag/v2025.8.29),
[Zero's TAF drop-in](https://content.taforever.com/featured_mod_dropins/tdraw-2025.4.24-Zero.zip),
[Escalation's](https://github.com/tanvanman/TADR/releases/tag/v2026.8.6),
[Twilight's effective TADR](https://github.com/tanvanman/TADR/releases/tag/v2026.6.8).

A strings cross-check found `Build menu rotation overlay`, the buildrotate GAF paths and the preview
machinery in Escalation and Twilight, and `Accessible Chat` and `ChatBackdrop` in Escalation only.
No native DLL contains a reload, status-counter, team-nano, mute, extended-chat or share-percent
signature, and every one of those source modules was introduced after each bundle's exact
release. Absence therefore rests on version history, with strings as the cross-check, not on a
missing string alone
([the new modules in the development diff](https://github.com/tanvanman/TADR/compare/v2026.8.6...dev-dcff5dd)).

| Feature | Mayhem | ProTA | Zero | Escalation | Twilight B98 | Mod enabled under shipped defaults and content? |
|---|---|---|---|---|---|---|
| Building/factory placement rotation | No | No | No | Yes | Yes | **No**: all mod content has zero active `Rotations=` entries |
| Directional build-menu overlays and click zones | No | No | No | Yes | Yes | **No visible use**: the checkbox defaults on, but no unit definition is rotatable |
| Nanoframe model preview before placement | No | No | No | Yes | Yes | **Yes, Escalation and Twilight**, independent of the rotation opt-in |
| Accessible Chat: black backdrop and legible font | No | No | No | Yes | No | **No by default**: Escalation's option defaults off and its shipped config does not override it |
| Reload bars for weapons | No | No | No | No | No | **No**; no active `reloadbar=` in content either |
| Stockpile and transport counters above health bars | No | No | No | No | No | **No**; the engine support is absent |
| Team-coloured construction, reclaim and nanoframe effects | No | No | No | No | No | **No**; the engine support is absent and no config opts in |
| Allied queued-build overlays | No | No | No | No | No | **No**; the engine support is absent |
| Movable chat anchor | No | No | No | No | No | **No**; the engine support is absent |
| Extended chat renderer: channel split, history, font controls | No | No | No | No | No | **No**; the engine support is absent |
| Per-player local mute for chat, pings and drawings | No | No | No | No | No | **No**; the engine support is absent |
| Percentage resource-sharing thresholds | No | No | No | No | No | **No**; the engine support is absent |

Sources for the matrix: [v2026.5.9, rotation and preview](https://github.com/tanvanman/TADR/releases/tag/v2026.5.9),
[v2026.7.3, accessible chat](https://github.com/tanvanman/TADR/releases/tag/v2026.7.3),
[v2026.8.6 dialog defaults](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/dialog.cpp#L146),
[v2026.6.8 preview mode](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/buildghost.h#L30),
[v2026.8.6 preview mode](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/buildghost.h#L42),
[the development diff](https://github.com/tanvanman/TADR/compare/v2026.8.6...dev-dcff5dd). The content
counts are in the last section.

- A supported mod hand-upgraded to a newer development DLL could expose more conveniences, but that
  is a different installation from the supported fixture and cannot turn a native-bundle No into
  a Yes.
- The building preview is in use even though rotation is not: in the Escalation and Twilight
  versions it defaults to a 3D model for any ordinary structure placement, and Escalation ships
  `NanoframePreviewFill=FALSE` to select the wireframe style.

**What this section does not show.** Player preferences saved in the registry can override the
default UI states; this describes shipped settings and clean defaults, not players' settings.
Zero's DLL comes in a mod-specific TAF package: it is pinned to the documented version and package
hash, not claimed byte-identical to the generic official v2025.4.24 DLL.

## Are the missing newer features released, or only in source?

The newer additions are absent from the supported bundles, but several of them have downloadable
**public development prereleases**, so calling them all unreleased git work would mislead. The
newest stable release is v2026.8.6; the releases API lists the newer `dev-*` and `latest` assets as
prereleases ([releases API](https://api.github.com/repos/tanvanman/TADR/releases),
[stable v2026.8.6](https://github.com/tanvanman/TADR/releases/tag/v2026.8.6)).

The dates below are the oldest prerelease still listed and verified during the pass, not a claim
that no earlier, transient CI artifact existed. A git commit date alone does not establish a public
release.

| Feature | Git introduction | Public downloadable evidence | Defaults and gates in the pinned dev source |
|---|---|---|---|
| Reload bars | `bd5119e`, 2026-08-08 | `dev-1615f13`, published 2026-08-30 11:35:23 UTC: its Escalation DLL has the `reloadbar` signature, and the install code calls `ReloadBars::Install()` | A weapon TDF's `reloadbar` must have its low bit set; there is no universal automatic bar |
| Stockpile and transport counters | `1739edd`, 2026-08-14 | The same Aug 30 DLL has both `ShowStockpileCounter` and `ShowTransportCounter`; the install call is verified | Both default TRUE in the DLLs that have them; they apply to your own units while health bars are on |
| Team-coloured nanolathe | `6a187c2`, 2026-08-13 | The same Aug 30 DLL has the `TeamColorNanolathe` signatures; the install call is verified | `TeamColorNanolathe=FALSE` by default; the player or the mod must opt in |
| Allied queued-build overlays | `268491a`, 2026-08-08 | The same Aug 30 DLL contains `ShowAlliedBuildQueues`; later September builds compile it out | `ALLIED_BUILD_QUEUE_ENABLE=0` in every config at `dev-dcff5dd`: no checkbox, packets or rendering compiled in |
| Movable chat and player mute | `72018b5`, authored 2026-08-16, merged Sep 13 | `dev-682ee2f`, published 2026-09-13 00:32:56 UTC: its Escalation DLL has `ChatAnchor`, `.mute` and the PlayerMute install diagnostics | With no ChatAnchor the engine's position stays. PlayerMute's compile gate is 1 in Escalation and 0 in the other configs; nobody starts muted |
| Extended chat renderer | `3bed4eb`, 2026-09-05, merged Sep 13 | The same Sep 13 Escalation DLL has the `ChatRenderer` diagnostics | `ChatRenderer=engine` by default; the custom renderer is optional, with chat split 0 and line and font controls 0 |
| Percentage resource-share thresholds | `46c3646`, 2026-09-07, merged Sep 9 | The same Sep 13 Escalation DLL has `[SharePercent] installed` | `SHARE_PERCENT_ENABLE=1` in Escalation, inherited 0 elsewhere; percent mode needs a percent argument to the command |

Primary sources: [reload bars](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/ReloadBars.cpp#L240),
[counter defaults](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/UnitStatusCounters.cpp#L355),
[team nano defaults](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/TeamColorNanolathe.cpp#L158),
[allied-queue compile gate](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/config.h#L107),
[Escalation compile gates](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/config_escalation.h#L160),
[player mute](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/PlayerMute.h),
[chat anchor defaults](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/ChatPosition.cpp#L264),
[extended chat defaults](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/ChatLayout.cpp#L759),
[percentage sharing](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/SharePercent.cpp#L178),
[the Aug 30 prerelease](https://github.com/tanvanman/TADR/releases/tag/dev-1615f13),
[the Sep 13 prerelease](https://github.com/tanvanman/TADR/releases/tag/dev-682ee2f).

- Rotation, the directional overlays and the model placement preview were already in stable
  v2026.5.9; Accessible Chat was first stable in v2026.7.3
  ([May 9](https://github.com/tanvanman/TADR/releases/tag/v2026.5.9),
  [July 3](https://github.com/tanvanman/TADR/releases/tag/v2026.7.3)).
- Every newer module was introduced after stable v2026.8.6, whose source tree lacks these files.
  The development `tdraw.txt` lists reload bars, counters, team nano and allied builds in its undated
  `????.??.??` section; that marks them as not yet in a stable changelog and does not mean no
  prerelease exists ([stable tree](https://github.com/tanvanman/TADR/tree/v2026.8.6/src/DDraw),
  [pinned development changelog](https://github.com/tanvanman/TADR/blob/dev-dcff5dd/src/DDraw/tdraw.txt#L65)).
- On 2026-10-04 the releases API listed a mutable `latest`, published 2026-10-01 08:25:55 UTC at
  tag object `27b822c8f4530b8034114379efef4cd2a11e59f6`. The `latest` tag in `vendor/TADR` is
  stale, so the gate claims here are pinned to the immutable Sep 20 `dev-dcff5dd` source
  ([latest release](https://api.github.com/repos/tanvanman/TADR/releases/tags/latest),
  [latest tag](https://api.github.com/repos/tanvanman/TADR/git/ref/tags/latest),
  [pinned Sep 20 source](https://github.com/tanvanman/TADR/tree/dev-dcff5dd)).

The coverage table therefore reports these rows as **not enabled by any mod**, **not in any bundled
TADR** and **publicly downloadable as a development prerelease**, rather than collapsing all three
into "unreleased". The allied-queue documentation describes a display that defaults on, but the
compile gate removes it in the later pinned configurations; a version's own gate source wins over
a generic feature description.

**What this section does not show.** The Oct 1 mutable `latest` source and every development DLL
configuration were not audited; native-bundle absence did not need them. The pinned compile gates
are dated accordingly and are not a verified description of `latest` on Oct 4. The first listed
prerelease was checked for the feature in its binary and its install in the source; nothing was
run in game.

## Which mod files opt into the features?

No supported fixture has an active `Rotations`, `reloadbar`, team-nano or status-counter
preference. Escalation does actively use one **placement-preview extension**,
`PreviewFaceOpponent=1`, in 12 unit definitions, although no definition allows manual rotation
([key documentation](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/tdraw.txt)).

Every non-empty `.hpi`, `.gp3`, `.ufo` and `.ccx` in each fixture was read with the archive reader
in `tools/hpipack.py`. Each archived `.fbi`, `.tdf` and `.ini` was decoded in memory; C block
comments, `//` line tails and lines starting with a semicolon comment were removed, and key
assignments were then searched case-insensitively after a newline, brace or semicolon boundary.
Semicolon assignment terminators were kept, so several keys on one line are all found. The loose
mod preferences and the final override layers (Zero, Twilight) were read as well. No extracted
original or mod data was written to disk.

| Fixture | FBI records | TDF records | Archived INI records | Active `Rotations` | Active `reloadbar` | Active preview extension | Parse errors |
|---|---:|---:|---:|---:|---:|---|---:|
| Mayhem 11.3.0 | 505 | 195 | 1 | 0 | 0 | none | 0 |
| ProTA 4.8 | 317 | 469 | 0 | 0 | 0 | none | 0 |
| Escalation GOLD 10.2.0 | 549 | 468 | 0 | 0 | 0 | `PreviewFaceOpponent=1` in 12 | 0 |
| Zero Alpha 5 | 269 | 610 | 0 | 0 | 0 | none | 0 |
| Zero TAZ31 120526 override | 269 | 610 | 0 | 0 | 0 | none | 0 |
| Twilight Beta 91 archive layer | 505 | 1087 | 0 | 0 | 0 | none | 0 |
| Twilight Beta 98 archive override | 504 | 791 | 0 | 0 | 0 | none | 0 |

These are archive record counts, not the effective merged unit counts; the Zero and Twilight
override copies are shown separately. Seven zero-length Twilight Beta 91 archives were confirmed
empty and skipped — they are intentional files blanking retail archives, and the manifest
documents them.

- The 12 Escalation units that opt into the preview facing are `armamb`, `armbrtha`, `armguard`,
  `armvulc`, `corbuzz`, `corint`, `corpun`, `cortoast`, `corspun`, `armsbertha`, `cornova` and
  `corbfg`. `PreviewFaceOpponent` sets the **ghost's** facing; it is not the whitelist for manual
  rotation of the building, and upstream's documentation keeps the two apart.
- The loose shipped configs read were `TAESC.ini`, `mayhem.ini`, `ProTA.ini`, Zero's `TAZero.ini`
  with its TADR layer `tazero.ini.tdraw`, and Twilight's Beta 98/final `totala.ini`. None assigns
  `TeamColorNanolathe`, `ShowStockpileCounter`, `ShowTransportCounter`, `ChatBackdrop`,
  `BuildMenuRotationOverlay`, `ChatAnchor` or `ChatRenderer`. Only Escalation sets
  `NanoframePreviewFill=FALSE`.
- The build-menu overlay checkbox starts at 1 in both stable v2026.6.8 and v2026.8.6. Accessible
  Chat starts at 0 in v2026.8.6, under the registry key `ChatBackdrop`. The widget reads a registry
  byte only when one exists, so otherwise those defaults stand
  ([Twilight's dialog](https://github.com/tanvanman/TADR/blob/v2026.6.8/src/DDraw/dialog.cpp#L73),
  [Escalation's dialog](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/dialog.cpp#L146),
  [the registry read](https://github.com/tanvanman/TADR/blob/v2026.8.6/src/DDraw/Widgets/Button.cpp#L96)).

"Rotation support shipped" is Yes for Escalation and Twilight; "used by their shipped unit
content" is No. The directional overlay has the same content prerequisite, although its checkbox
defaults on. The placement preview, on the other hand, is used content: Escalation's 12 facing keys
customize it. Impure's ghosts cover the generic preview, and the extra keys are the partial row in
[what Impure has](quality-of-life-impure.md).

**What this section does not show.** Escalation's only `Rotations` examples, in `corgant` and
`corint`, are commented out; this scan counts active keys and does not read comments as opt-ins.
Factory COB scripts may turn their own models; that scripted behaviour is not TADR's manual
placement rotation and is not classified as it.
