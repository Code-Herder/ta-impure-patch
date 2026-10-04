# TADR releases, native bundles and build configurations

**Sources:** pinned release assets and native compatibility fixtures audited 2026-10-04;
[release history](https://github.com/tanvanman/TADR/releases);
[development configuration snapshot](https://github.com/tanvanman/TADR/tree/dev-dcff5dd/src/DDraw).

## Native bundles are older than development configurations

The supported mods do not all ship the same TADR. Native **Mayhem 11.3.0 uses v2024.3.31**,
**ProTA 4.8 v2025.8.29**, **Zero Alpha 5 + TAZ31 120526 the TAF 2025.4.24 Zero build**,
**Escalation GOLD 10.2.0 v2026.8.6-esc**, and **Twilight Beta 98 v2026.6.8-full**.
The [QoL coverage table](tadr-port/quality-of-life.md) records exact native DLL hashes, feature
inclusion and mod activation. These versions include effective final drop-in layers.

Rotation and directional build-menu choices are released features, present in native Escalation
and Twilight, but no checked mod has active `Rotations=` definitions. Escalation uses the
separate `PreviewFaceOpponent=1` key in 12 units. ProTA's 2025 DLL has neither rotation nor allied
queue overlays. Later upstream configuration names do not establish native shipment.

There has been no official Cavedog patch since v3.1 in 1998; the releases below are community
work. The table describes fixed snapshots, not the mutable `latest` development download.

## Selected releases relevant to QoL coverage

| Release / public publication | Relevant content |
|---|---|
| [dev-dcff5dd](https://github.com/tanvanman/TADR/releases/tag/dev-dcff5dd), 2026-09-20 | Pinned configuration source below; allied build queues compiled out of every configuration |
| [dev-682ee2f](https://github.com/tanvanman/TADR/releases/tag/dev-682ee2f), 2026-09-13 | Verified Escalation download contains chat anchor/renderer, local mute and percentage sharing |
| [dev-1615f13](https://github.com/tanvanman/TADR/releases/tag/dev-1615f13), 2026-08-30; build label 2026.8.29 | Verified Escalation download contains reload bars, status counters, team-coloured nanolathe and allied queue feature signatures |
| [v2026.8.6](https://github.com/tanvanman/TADR/releases/tag/v2026.8.6) | Native Escalation bundle; dithering toggle, repair-rate fix, full/wireframe nanopreview option and replay POV visibility |
| v2026.8.1 | Escalation share guard; enhanced built-in minimap palette; BTA build configuration |
| v2026.7.27 | `nomapweaponalert`; dedicated Zero configuration; megamap dithering |
| [v2026.7.3](https://github.com/tanvanman/TADR/releases/tag/v2026.7.3) | Accessible Chat option; improved megamap features and nano preview |
| v2026.6.23 | Fixed feature rendering on megamap; extended weapon IDs disabled pending testing |
| v2026.6.17 | Extended weapon ID support introduced |
| [v2026.6.8](https://github.com/tanvanman/TADR/releases/tag/v2026.6.8) | Effective native Twilight full build; old clock/weather surface-pointer guard corrected |
| [v2026.5.9](https://github.com/tanvanman/TADR/releases/tag/v2026.5.9) | First stable building rotation, directional build-menu choices and model placement previews |
| [v2025.8.29](https://github.com/tanvanman/TADR/releases/tag/v2025.8.29) | Native ProTA bundle; predates rotation and newer display/chat modules |
| [v2024.3.31](https://github.com/tanvanman/TADR/releases/tag/v2024.3.31) | Native Mayhem bundle |

The newer conveniences are **not all unreleased git work**: the fixed development assets above
were downloaded and inspected. They are absent from all five native mod bundles, and a later
compile gate can remove a feature that an earlier development download included. Development
availability does not establish default activation or a tested gameplay result.

## Pinned development build configurations

These are **six selected build targets at `dev-dcff5dd`**, not the DLLs bundled with those mods.
The snapshot also has a Twilight target. Every column selects a different build of shared
upstream code. **✅ means the feature/hook is compiled or installed by that configuration**, not
that the mod's content or fresh preferences activate every behavior. For the native installations
and actual opt-ins, use [the coverage audit](tadr-port/quality-of-life.md).

| Feature | ProTA | Escalation | Mayhem | TA:Zero | BTA | OTA |
|---|---|---|---|---|---|---|
| **Build / placement** ||||||
| Mex snap (max radius) | 3 | — | 3 | 3 | 1 | — |
| Wreck snap (max radius) | 1 | 1 | 1 | 1 | 1 | — |
| Guarding constructors: stay/scatter options | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Constructor patrol mode options | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Auto-kickout of con units blocking a builder | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| **HUD / display** ||||||
| TA hook UI enhancements | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Megamap | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Map features on megamap (mexes, geos, spires, walls) | ✅ | ✅ | ✅ | ✅ | — | — |
| Whiteboard | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Weather report — game clock | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Weather report — wind row | ✅ | ✅ | ✅ | — | ✅ | — |
| Weather report — tidal row | ✅ | ✅ | ✅ | — | ✅ | — |
| Map-DT owner-11 draw hook included | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| **Sim / netcode** ||||||
| Wind speed synced between players | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| `.take` claim arbitration (per-target claims + election) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Lag-switch mitigation (freeze sim when network goes dark) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Share-abuse guard (share rate limit, `.take` vs dead commander) | — | ✅ | — | — | — | — |
| **Combat** ||||||
| Off-map aircraft targetable, tiles past map edge | 1 | **32** | **32** | 1 | 1 | 1 |
| Air splash hits every stacked aircraft on a cell | ✅ | ✅ | ✅ | — | — | — |
| Contested-cell tie-break by unit index | ✅ | ✅ | ✅ | — | — | — |
| Repair-rate exploit fix | — | ✅ (3×/3×) | — | — | — | — |
| Air wrecks over land fall to the ground | — | ✅ | — | — | — | — |
| Extended weapon IDs (≥ 256) | — | ✅ | ✅ | — | — | — |

## Interpreting the configuration matrix

The map-DT row describes a draw hook, not proven visibility. It needs owner-11 markers; upstream's
marker writes are commented out and only native ProTA supplies them through its loader.
Guarding options default to Cavedog behavior; available assist-only patrol choices do not imply
that assist-only is the fresh default. The new Zero configuration disables wind/tidal readouts,
whereas native Zero's older DLL includes that renderer.

The 32-tile off-map aircraft margin in the Mayhem development target is real source behavior;
it is not evidence that Mayhem 11.3.0's v2024.3.31 DLL ships it. A mod INI is only one layer of
feature evidence, and a newer `config_mayhem.h` is another version altogether.

Escalation's target includes share guard, repair-rate changes and falling aircraft wrecks;
extended weapon IDs are enabled in **both Escalation and Mayhem** in this snapshot. Those are
upstream implementations, rather than an Escalation engine fork ([the Escalation deep dive](deep-ta-esc.md)).
The v2026.6.17 → v2026.6.23 weapon-ID sequence shows why version-specific gates matter.
Defaults in `config.h` remain off unless a target explicitly enables them.
