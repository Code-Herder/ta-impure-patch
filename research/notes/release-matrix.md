# Release Timeline & Per-Mod Feature Matrix

**Source:** [github.com/tanvanman/TADR/releases](https://github.com/tanvanman/TADR/releases), read directly 2026-08-31. [VERIFIED]

## Summary

The last unofficial Total Annihilation patch is **`v2026.8.6`**, released **6 August 2026**. Development
builds continue past it — `dev-1615f13` (tdraw and recorder builds, 2026.8.29) and `dev-13d71dd`,
dated **31 August 2026**, which installs "instrumentation and breadcrumb ring buffers to diagnose a
stack-skewing orders bug". The patch line is actively maintained, not archival.

There has been no *official* patch since Cavedog's v3.1 in 1998. Everything since is community work.

## Release cadence, 2026

| Release | Notable content |
|---|---|
| `dev-13d71dd` (2026-08-31) | Instrumentation / breadcrumb ring buffers for a stack-skewing orders bug |
| `dev-1615f13` (2026-08-29) | Updated tdraw and recorder builds |
| **`v2026.8.6`** (latest stable) | Dithering toggle in `totala.ini`; index-keyed repair-rate fix (Escalation only, by RdKL); full vs wireframe nanopreview option; demo-from-player-POV hides units outside that player's LOS/radar |
| `v2026.8.1` | Escalation share guard; enhanced built-in minimap palette (TAG_Venom); BTA build config |
| `v2026.7.27` | `nomapweaponalert` weapon tag (Scott Warley); TA:Zero build config; megamap dithering |
| `v2026.7.17` | Fix player rejection when watching a replay |
| `v2026.7.3` | Nano preview shimmer wire; antialiased/alpha-blended mexes and features on megamap; accessible chat option |
| `v2026.6.23` | Metal patch + fixed feature rendering on megamap; **extended weapon ID support disabled pending further testing** |
| `v2026.6.17` | Extended weapon ID patch introduced |
| `v2026.6.13` | Linux (Twilight) crash fix; richer crash reports; ATL/MFC dependencies removed |

The 6.17 → 6.23 sequence is a useful data point on risk: extended weapon IDs shipped, then were
switched off one release later pending testing. This corroborates the finding that
`WeaponIdOverflow` defaults to off.

## Per-mod feature matrix

One DLL serves six games through build configurations. Columns are the shipped configs. [VERIFIED]

| Feature | ProTA | Escalation | Mayhem | TA:Zero | BTA | OTA |
|---|---|---|---|---|---|---|
| **Build / placement** ||||||
| Mex snap (max radius) | 3 | — | 3 | 3 | 1 | — |
| Wreck snap (max radius) | 1 | 1 | 1 | 1 | 1 | — |
| Construction units stay put while guarding | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Con units reclaim-only / assist-only while patrolling | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Auto-kickout of con units blocking a builder | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| **HUD / display** ||||||
| TA hook UI enhancements | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Megamap | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Map features on megamap (mexes, geos, spires, walls) | ✅ | ✅ | ✅ | ✅ | — | — |
| Whiteboard | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Weather report — game clock | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Weather report — wind row | ✅ | ✅ | ✅ | — | ✅ | — |
| Weather report — tidal row | ✅ | ✅ | ✅ | — | ✅ | — |
| Map DTs always visible | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| **Sim / netcode** ||||||
| Wind speed synced between players | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| `.take` claim arbitration (per-target claims + election) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Lag-switch mitigation (freeze sim when network goes dark) | ✅ | ✅ | ✅ | ✅ | — | ✅ |
| Share-abuse guard (share rate limit, `.take` vs dead commander) | — | ✅ | — | — | — | — |
| **Combat** ||||||
| Off-map aircraft targetable, tiles past map edge | 1 | **32** | **32** | 1 | 1 | 1 |
| Air splash hits every stacked aircraft on a cell | ✅ | ✅ | ✅ | — | — | — |
| Contested-cell tie-break by unit index | ✅ | ✅ | ✅ | — | — | — |
| Repair-rate exploit fix | — | ✅ (3×/3×) | — | — | — | — |
| Air wrecks over land fall to the ground | — | ✅ | — | — | — | — |
| Extended weapon IDs (≥ 256) | — | ✅ | ✅ | — | — | — |

## What the matrix tells us

**"Off-map AA" is real.** The Mayhem deep dive found no evidence for it in that mod's INI patch set
and reported it as unsubstantiated. It exists — but as a `tdraw` build-config feature (32 tiles past
the map edge for Escalation and Mayhem, versus 1 for everyone else), not as a Mayhem byte patch.
The lesson generalises: a mod's INI is not the full inventory of what it ships, because the shared
DLL carries per-mod behaviour too.

**Escalation is the most-served config, and still authors nothing.** Share guard, repair-rate fix,
falling air wrecks and extended weapon IDs are all Escalation-only — implemented upstream by the
TADR maintainers at Wotan's request. This is the clearest confirmation of the
"specifies, does not implement" relationship. See [[deep-ta-esc]].

**Stock OTA is deliberately the most conservative column.** Competitive-neutral features (megamap,
whiteboard, `.take` arbitration, lag-switch mitigation) are enabled everywhere; anything that
changes the simulation is gated per-mod. That gating is how one DLL avoids splitting the multiplayer
population — see [[deep-tadr]].
